#include "esp_sdcard_log.hpp"
#include "offsets.hpp"
#include "ui_log.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

static std::string g_logDir;
static std::string g_fullPath;
static std::chrono::steady_clock::time_point g_lastTickWrite{};
static pid_t g_cachedPid = 0;
static EspResolveReport g_cachedRep{};
static ConrentResolve g_cachedCr{};
static RemoteIl2CppInfo g_cachedIl2{};

static std::string self_package() {
    char packageName[256] = {};
    FILE *fp = std::fopen("/proc/self/cmdline", "r");
    if (fp) {
        std::fread(packageName, 1, sizeof(packageName) - 1, fp);
        std::fclose(fp);
    }
    return packageName;
}

static std::string choose_log_dir() {
    const std::string pkg = self_package();
    std::vector<std::string> candidates;
    if (!pkg.empty()) {
        candidates.push_back("/storage/emulated/0/Android/data/" + pkg + "/files");
        candidates.push_back("/sdcard/Android/data/" + pkg + "/files");
        candidates.push_back("/data/data/" + pkg + "/files");
        candidates.push_back("/data/user/0/" + pkg + "/files");
    }
    candidates.push_back("/storage/emulated/0/Download");
    candidates.push_back("/sdcard/Download");

    for (const std::string &dir : candidates) {
        const std::string probe = dir + "/.mlbb_esp_write_probe";
        FILE *out = std::fopen(probe.c_str(), "w");
        if (!out) continue;
        std::fclose(out);
        std::remove(probe.c_str());
        return dir;
    }
    return {};
}

static void write_atomic(const std::string &path, const std::string &text) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << text;
    }
    std::rename(tmp.c_str(), path.c_str());
}

static void append_log(const std::string &dir, const std::string &text) {
    std::ofstream log(dir + "/log.txt", std::ios::app);
    if (log) log << text;
}

static void hex(std::ostringstream &os, uintptr_t v) {
    os << "0x" << std::hex << v << std::dec;
}

static void field_line(std::ostringstream &os, const char *owner, const char *name,
                       uintptr_t runtimeOff, uintptr_t compileOff) {
    os << "  " << owner << "." << name << "  runtime=";
    if (runtimeOff)
        hex(os, runtimeOff);
    else
        os << "(missing)";
    os << "  compile(Off::)=";
    hex(os, compileOff);
    if (runtimeOff && compileOff && runtimeOff != compileOff)
        os << "  *** MISMATCH ***";
    os << "\n";
}

static std::string build_how_esp_works() {
    std::ostringstream os;
    os << "===== HOW MLBB ESP PURE WORKS (conRentV1 external port) =====\n";
    os << "1. Overlay attaches to game PID (UnityKillsMe / MLBB).\n";
    os << "2. Resolve: ptrace IL2CPP in target — FindClass(BattleManager), static Instance.\n";
    os << "3. Runtime field offsets via il2cpp_class_get_field_from_name + field_get_offset.\n";
    os << "4. ESP tick (reader thread): vm_read BattleManager + dict/list of ShowEntity.\n";
    os << "5. Filter: skip m_bDeath, skip m_bSameCampType (enemies only).\n";
    os << "6. Position: ShowEntity LastPosition (fallback CachePosition in reader).\n";
    os << "7. W2S: IL2 Camera.WorldToScreenPoint via runtime_invoke on reader thread.\n";
    os << "8. GL overlay draws boxes/lines from cached screen coords (no draw-time ptrace).\n";
    os << "9. offsets.hpp Off:: values are compile defaults; runtime column is live IL2CPP.\n\n";
    return os.str();
}

static std::string build_resolve_section(pid_t pid, const EspResolveReport &rep,
                                         const ConrentResolve &cr, const RemoteIl2CppInfo &il2) {
    std::ostringstream os;
    os << "===== RESOLVE @ pid=" << pid << " =====\n";
    os << "status=" << (int)rep.status << " detail=" << rep.detail << "\n";
    os << "maps=" << rep.mapsCount << " readProbe=" << (rep.readProbe ? "OK" : "FAIL") << "\n";
    os << "il2cpp lib=" << rep.il2cppLib << " base=";
    hex(os, rep.il2cppBase);
    os << " domain_get=";
    hex(os, rep.il2cppDomainGet);
    os << " domain*=";
    hex(os, rep.il2cppDomainPtr);
    os << "\n";
    if (rep.il2cppNote[0]) os << "il2cpp note: " << rep.il2cppNote << "\n";
    os << "conRent method: " << cr.method << "\n";
    os << "BattleManager klass=";
    hex(os, cr.battle_manager_klass);
    os << " statics=";
    hex(os, cr.battle_manager_statics);
    os << " Instance=";
    hex(os, cr.battle_manager);
    os << "\n";
    os << "ShowBattleCamera statics=";
    hex(os, cr.show_battle_camera_statics);
    os << " GameMethod statics=";
    hex(os, cr.game_method_statics);
    os << "\n";
    os << "Camera get_main MethodInfo=";
    hex(os, cr.camera_get_main_method);
    os << " native=";
    hex(os, cr.camera_get_main_native);
    os << "\n";
    os << "Camera WorldToScreenPoint MethodInfo=";
    hex(os, cr.camera_w2s_method);
    os << " native=";
    hex(os, cr.camera_w2s_native);
    os << "\n";
    os << "metadata_ready=" << (cr.metadata_ready ? "yes" : "no") << "\n\n";

    const ConrentRuntimeOffsets &rt = cr.runtime_fields;
    os << "===== RUNTIME FIELD OFFSETS (IL2CPP) vs Off:: compile =====\n";
    field_line(os, "BattleManager", "m_LocalPlayerShow", rt.bm_m_LocalPlayerShow,
               Off::LocalPlayerShow);
    field_line(os, "BattleManager", "m_dicPlayerShow", rt.bm_m_dicPlayerShow, Off::DicPlayerShow);
    field_line(os, "BattleManager", "m_dicMonsterShow", rt.bm_m_dicMonsterShow,
               Off::DicMonsterShow);
    field_line(os, "BattleManager", "m_ShowPlayerList", rt.bm_m_ShowPlayerList,
               Off::ListPlayerShow);
    field_line(os, "BattleManager", "m_ShowMonsterList", rt.bm_m_ShowMonsterList,
               Off::ListMonsterShow);
    os << "ShowEntity klass=";
    hex(os, rt.show_entity_klass);
    os << "\n";
    field_line(os, "ShowEntity", "LastPosition", rt.se_LastPosition, Off::LastPosition);
    field_line(os, "ShowEntity", "CachePosition", rt.se_CachePosition, Off::CachePosition);
    field_line(os, "ShowEntity", "m_ID", rt.se_m_ID, Off::EntityId);
    field_line(os, "ShowEntity", "m_bDeath", rt.se_m_bDeath, Off::EntityDeath);
    field_line(os, "ShowEntity", "m_bSameCampType", rt.se_m_bSameCampType, Off::SameCamp);
    field_line(os, "ShowEntity", "m_Hp", rt.se_m_Hp, Off::Hp);
    field_line(os, "ShowEntity", "m_HpMax", rt.se_m_HpMax, Off::HpMax);

    os << "\n===== Off:: LAYOUT HELPERS (compile-only) =====\n";
    os << "  DictEntries=";
    hex(os, Off::DictEntries);
    os << " DictCount=";
    hex(os, Off::DictCount);
    os << " EntryStride=";
    hex(os, Off::EntryStride);
    os << " EntryValue=";
    hex(os, Off::EntryValue);
    os << "\n";
    os << "  ListItems=";
    hex(os, Off::ListItems);
    os << " ListSize=";
    hex(os, Off::ListSize);
    os << " ArrData=";
    hex(os, Off::ArrData);
    os << "\n";
    os << "  KlassName=";
    hex(os, Off::KlassName);
    os << " StaticFieldOffs[]={0xB8,0xC0,0xB0,...}\n";

    os << "\n===== IL2CPP API POINTERS (in game) =====\n";
    os << "  class_from_name=";
    hex(os, il2.sym_class_from_name);
    os << " field_get_offset=";
    hex(os, il2.sym_field_get_offset);
    os << " runtime_invoke=";
    hex(os, il2.sym_runtime_invoke);
    os << "\n\n";
    return os.str();
}

static std::string build_live_section(const EspFrame &frame, bool espDrawActive) {
    std::ostringstream os;
    os << "===== LIVE TICK =====\n";
    os << "esp_draw=" << (espDrawActive ? "ON" : "OFF") << " status=" << (int)frame.status
       << " hint=" << frame.hint << "\n";
    os << "self world=(" << frame.selfX << "," << frame.selfY << "," << frame.selfZ << ")\n";
    os << "self screen valid=" << (frame.selfScreenValid ? "yes" : "no");
    if (frame.selfScreenValid)
        os << " (" << frame.selfScreenX << "," << frame.selfScreenY << ")";
    os << "\n";
    os << "bmInstance=";
    hex(os, frame.bmInstance);
    os << " playerDict=";
    hex(os, frame.playerDict);
    os << " monsterDict=";
    hex(os, frame.monsterDict);
    os << " counts P" << frame.dictPlayers << " M" << frame.dictMonsters << "\n";
    os << "targets=" << frame.targets.size() << "\n";
    for (size_t i = 0; i < frame.targets.size() && i < 32; ++i) {
        const EspTarget &t = frame.targets[i];
        os << "  [" << i << "] ent=";
        hex(os, t.ent);
        os << " kind=" << t.kind << " " << t.label << " hp=" << t.hp << "/" << t.hpMax
           << " m=" << t.meters << " world=(" << t.worldX << "," << t.worldY << "," << t.worldZ
           << ")";
        if (t.screenValid)
            os << " screen=(" << t.screenX << "," << t.screenY << ")";
        else
            os << " screen=invalid";
        os << "\n";
    }
    if (frame.targets.size() > 32) os << "  ... (" << (frame.targets.size() - 32) << " more)\n";
    os << "\n";
    return os.str();
}

static void publish_files(const std::string &body, bool appendLiveOnly) {
    const std::string dir = g_logDir.empty() ? choose_log_dir() : g_logDir;
    if (dir.empty()) {
        ui_log("[sdcard] cannot write ESP log (no writable dir)");
        return;
    }
    g_logDir = dir;
    g_fullPath = dir + "/mlbb_esp_full_log.txt";
    const std::string resolvePath = dir + "/esp_resolve.txt";

    write_atomic(g_fullPath, body);
    if (!appendLiveOnly) {
        /* conRentV1 compatible short file */
        const size_t livePos = body.find("===== LIVE TICK =====");
        const std::string shortTxt =
            livePos == std::string::npos ? body : body.substr(0, livePos);
        write_atomic(resolvePath, shortTxt);
        append_log(dir, "\n===== MLBB ESP RESOLVE =====\n" + shortTxt + "\n");
    }

    ui_log("[sdcard] ESP log -> %s", g_fullPath.c_str());
}

} // namespace

const char *esp_sdcard_log_dir() { return g_logDir.c_str(); }

const char *esp_sdcard_log_full_path() { return g_fullPath.c_str(); }

void esp_sdcard_log_after_resolve(pid_t pid, const EspResolveReport &rep, const ConrentResolve &cr,
                                  const RemoteIl2CppInfo &il2) {
    g_cachedPid = pid;
    g_cachedRep = rep;
    g_cachedCr = cr;
    g_cachedIl2 = il2;

    std::ostringstream body;
    body << "MLBB ESP PURE — full diagnostic log\n";
    body << "package=" << self_package() << "\n\n";
    body << build_how_esp_works();
    body << build_resolve_section(pid, rep, cr, il2);
    body << "===== LIVE TICK =====\n(pending — Start ESP in match)\n\n";
    publish_files(body.str(), false);
    g_lastTickWrite = {};
}

void esp_sdcard_log_after_tick(pid_t pid, const EspFrame &frame, bool espDrawActive) {
    if (pid <= 0 || frame.status != EspStatus::Ready) return;
    const auto now = std::chrono::steady_clock::now();
    if (g_lastTickWrite.time_since_epoch().count() != 0 &&
        now - g_lastTickWrite < std::chrono::seconds(2))
        return;
    g_lastTickWrite = now;

    const EspResolveReport &rep =
        g_cachedPid == pid ? g_cachedRep : esp_last_resolve_report();
    const ConrentResolve &cr = g_cachedCr;
    const RemoteIl2CppInfo &il2 = g_cachedIl2;

    std::ostringstream body;
    body << "MLBB ESP PURE — full diagnostic log\n";
    body << "package=" << self_package() << " pid=" << pid << "\n\n";
    body << build_how_esp_works();
    if (g_cachedPid == pid && cr.battle_manager_klass)
        body << build_resolve_section(pid, rep, cr, il2);
    else
        body << build_resolve_section(pid, rep, ConrentResolve{}, il2);
    body << build_live_section(frame, espDrawActive);
    publish_files(body.str(), true);
}
