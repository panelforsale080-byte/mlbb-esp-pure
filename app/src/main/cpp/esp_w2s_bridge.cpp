#include "esp_w2s_bridge.hpp"
#include "esp_inject.hpp"
#include "mem.hpp"
#include "ui_log.hpp"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>

namespace {

constexpr uint32_t kMagic = 0x48533257u;
constexpr int kMaxPts = 128;

struct EspW2sShm {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t req_seq;
    volatile uint32_t ack_seq;
    float screen_w;
    float screen_h;
    uint32_t count;
    float wx[kMaxPts];
    float wy[kMaxPts];
    float wz[kMaxPts];
    float sx[kMaxPts];
    float sy[kMaxPts];
    uint8_t ok[kMaxPts];
};

static std::mutex g_mu;
static pid_t g_pid = 0;
static uintptr_t g_remoteShm = 0;
static uintptr_t g_getMainNative = 0;
static uintptr_t g_w2sNative = 0;
static bool g_injected = false;
static uint32_t g_seq = 0;
static std::string g_agentPath;

static std::string maps_path_for_lib(const char *libFileName) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(libFileName) == std::string::npos) continue;
        const char *path = std::strchr(line.c_str(), '/');
        if (!path) continue;
        std::string full(path);
        if (full.find(libFileName) != std::string::npos) return full;
    }
    return {};
}

static std::string agent_so_path() {
    if (!g_agentPath.empty()) return g_agentPath;
    const std::string mainLib = maps_path_for_lib("libMlbbEsp.so");
    if (!mainLib.empty()) {
        const auto pos = mainLib.find("libMlbbEsp.so");
        if (pos != std::string::npos) {
            const std::string sibling = mainLib.substr(0, pos) + "libmlbb_espw2s.so";
            std::ifstream probe(sibling, std::ios::binary);
            if (probe.good()) return sibling;
        }
    }
    const std::string agent = maps_path_for_lib("libmlbb_espw2s.so");
    if (!agent.empty()) return agent;
    return {};
}

static bool read_remote_shm_addr(pid_t pid, uintptr_t &out) {
    char path[128];
    std::snprintf(path, sizeof(path), "/data/local/tmp/mlbb_esp_%d.shm", (int)pid);
    std::ifstream f(path);
    if (!f) return false;
    unsigned long long v = 0;
    f >> std::hex >> v;
    if (!v || !mem_ptr_ok((uintptr_t)v)) return false;
    out = (uintptr_t)v;
    return true;
}

static bool probe_magic(pid_t pid, uintptr_t addr) {
    uint32_t magic = 0;
    return mem_read(pid, addr, magic) && magic == kMagic;
}

} // namespace

void esp_w2s_bridge_set_agent_path(const char *absolutePath) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_agentPath = absolutePath ? absolutePath : "";
}

void esp_w2s_bridge_reset() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_pid = 0;
    g_remoteShm = 0;
    g_getMainNative = 0;
    g_w2sNative = 0;
    g_injected = false;
    g_seq = 0;
}

void esp_w2s_bridge_publish_cfg(pid_t pid, uintptr_t get_main_native, uintptr_t w2s_native,
                                float screenW, float screenH) {
    if (pid <= 0 || !get_main_native || !w2s_native) return;
    char path[128];
    std::snprintf(path, sizeof(path), "/data/local/tmp/mlbb_esp_%d.cfg", (int)pid);
    FILE *f = std::fopen(path, "w");
    if (!f) return;
    std::fprintf(f, "get_main=0x%llx\n", (unsigned long long)get_main_native);
    std::fprintf(f, "w2s=0x%llx\n", (unsigned long long)w2s_native);
    std::fprintf(f, "screen_w=%.1f\n", screenW);
    std::fprintf(f, "screen_h=%.1f\n", screenH);
    std::fclose(f);
    std::lock_guard<std::mutex> lock(g_mu);
    g_pid = pid;
    g_getMainNative = get_main_native;
    g_w2sNative = w2s_native;
}

bool esp_w2s_bridge_ensure_injected(pid_t pid, const char *agentSoPath) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_pid == pid && g_remoteShm && probe_magic(pid, g_remoteShm)) return true;

    std::string soStorage;
    if (agentSoPath && agentSoPath[0])
        soStorage = agentSoPath;
    else
        soStorage = agent_so_path();
    if (soStorage.empty()) {
        ui_log("[inject] W2S agent failed: libmlbb_espw2s.so not found (nativeLibraryDir?)");
        return false;
    }
    char err[160] = {};
    if (!esp_inject_dlopen_so(pid, soStorage.c_str(), err, sizeof(err))) {
        ui_log("[inject] W2S agent failed: %s", err);
        return false;
    }
    ui_log("[inject] W2S agent: %s", err);

    for (int i = 0; i < 40; ++i) {
        uintptr_t addr = 0;
        if (read_remote_shm_addr(pid, addr) && probe_magic(pid, addr)) {
            g_pid = pid;
            g_remoteShm = addr;
            g_injected = true;
            ui_log("[inject] IL2 W2S shm=0x%llx (build #49 in-process)", (unsigned long long)addr);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ui_log("[inject] agent loaded but shm not ready");
    return false;
}

bool esp_w2s_bridge_active() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_pid > 0 && g_remoteShm && g_getMainNative && g_w2sNative;
}

void esp_w2s_bridge_set_screen(float screenW, float screenH) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_remoteShm || g_pid <= 0) return;
    EspW2sShm patch{};
    patch.screen_w = screenW;
    patch.screen_h = screenH;
    mem_write(g_pid, g_remoteShm + offsetof(EspW2sShm, screen_w), &patch.screen_w,
              sizeof(patch.screen_w));
    mem_write(g_pid, g_remoteShm + offsetof(EspW2sShm, screen_h), &patch.screen_h,
              sizeof(patch.screen_h));
}

bool esp_w2s_bridge_world_to_screen(pid_t pid, float wx, float wy, float wz, float &outX,
                                    float &outY) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (pid <= 0 || !g_remoteShm || g_pid != pid) return false;

    const uint32_t one = 1;
    const uint32_t seq = ++g_seq;
    mem_write(pid, g_remoteShm + offsetof(EspW2sShm, count), &one, sizeof(one));
    mem_write(pid, g_remoteShm + offsetof(EspW2sShm, wx), &wx, sizeof(wx));
    mem_write(pid, g_remoteShm + offsetof(EspW2sShm, wy), &wy, sizeof(wy));
    mem_write(pid, g_remoteShm + offsetof(EspW2sShm, wz), &wz, sizeof(wz));
    mem_write(pid, g_remoteShm + offsetof(EspW2sShm, req_seq), &seq, sizeof(seq));

    uint32_t ack = 0;
    for (int i = 0; i < 30; ++i) {
        mem_read(pid, g_remoteShm + offsetof(EspW2sShm, ack_seq), ack);
        if (ack == seq) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (ack != seq) return false;
    uint8_t ok = 0;
    mem_read(pid, g_remoteShm + offsetof(EspW2sShm, ok), ok);
    if (!ok) return false;
    mem_read(pid, g_remoteShm + offsetof(EspW2sShm, sx), outX);
    mem_read(pid, g_remoteShm + offsetof(EspW2sShm, sy), outY);
    return std::isfinite(outX) && std::isfinite(outY);
}
