#include "ui_core.hpp"
#include "esp_engine.hpp"
#include "esp_draw.hpp"
#include "proc.hpp"
#include "ui_log.hpp"
#include "clip_android.hpp"
#include "remote_il2cpp.hpp"

#include <android/input.h>
#include <android/log.h>
#include <GLES2/gl2.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"

static int g_width = 0, g_height = 0;
static int g_displayW = 0, g_displayH = 0;
ImVec2 initialScreenSize;
ImVec2 &ui_initial_screen_size() { return initialScreenSize; }

static bool g_glReady = false;
static bool g_imguiBackend = false;
static std::mutex g_glMu;
static const char *g_glslVersion = "#version 100";

static std::atomic<bool> g_runReader{false};
static std::thread g_reader;
static std::atomic<bool> g_readerStarted{false};
static std::mutex g_frameMu;
static EspFrame g_frame;
static std::atomic<pid_t> g_wantPid{0};
static std::atomic<bool> g_needResolve{true};

static bool g_autoFind = true;
static bool g_fallbackPid = true;
static bool g_showMenu = false;
static bool g_logScrollBottom = true;
static EspFeatures g_feat{};

static void apply_menu_style() {
    ImGuiStyle &s = ImGui::GetStyle();
    ImVec4 *c = s.Colors;
    s.WindowRounding = 10.f;
    s.FrameRounding = 6.f;
    s.WindowPadding = ImVec2(10.f, 10.f);
    s.WindowBorderSize = 1.f;
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.92f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.05f, 0.35f, 0.55f, 1.f);
    c[ImGuiCol_Button] = ImVec4(0.10f, 0.45f, 0.68f, 0.70f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.10f, 0.65f, 0.95f, 1.f);
    c[ImGuiCol_CheckMark] = ImVec4(0.06f, 0.80f, 1.f, 1.f);
}

static const char *status_name(EspStatus s) {
    switch (s) {
    case EspStatus::Idle: return "Idle";
    case EspStatus::NoProcess: return "NoProcess";
    case EspStatus::NoRead: return "NoRead";
    case EspStatus::Resolving: return "Resolving";
    case EspStatus::NoBattleManager: return "NoBattleManager";
    case EspStatus::NoInstance: return "NoInstance";
    case EspStatus::Ready: return "Ready";
    }
    return "?";
}

static void log_resolve_result(const EspResolveReport &r) {
    ui_log("[resolve] %s | %s", status_name(r.status), r.detail);
    ui_log("[il2cpp] lib=%s base=0x%llx domain_get=0x%llx domain*=0x%llx",
           r.il2cppLib[0] ? r.il2cppLib : "(none)",
           (unsigned long long)r.il2cppBase,
           (unsigned long long)r.il2cppDomainGet,
           (unsigned long long)r.il2cppDomainPtr);
    if (r.il2cppNote[0]) ui_log("[il2cpp] %s", r.il2cppNote);
    if (r.status == EspStatus::NoRead)
        ui_log("[resolve] FIX: enable root / run app as root / same user as MLBB");
    if (r.status == EspStatus::NoBattleManager)
        ui_log("[resolve] FIX: be IN MATCH; strings=%d (klass walk failed)", r.bmNameStrings);
    if (r.status == EspStatus::NoInstance)
        ui_log("[resolve] klass OK but Instance NULL — enter training/classic match then Resolve again");
}

static void log_il2cpp_scan(pid_t pid) {
    RemoteIl2CppInfo il2{};
    if (!remote_detect_il2cpp(pid, il2)) {
        ui_log("[il2cpp] scan: no module exports il2cpp_domain_get");
        return;
    }
    ui_log("[il2cpp] scan OK path=%s", il2.libPath);
    ui_log("[il2cpp] base=0x%llx domain_get=0x%llx class_from_name=%d assemblies=%d",
           (unsigned long long)il2.base, (unsigned long long)il2.sym_domain_get,
           il2.export_class_from_name ? 1 : 0, il2.export_domain_get_assemblies ? 1 : 0);
    ui_log("[il2cpp] %s", il2.note);
}

static void scan_and_log_processes() {
    ProcCandidate list[16];
    const size_t n = proc_list_mlbb(list, 16);
    ui_log("[scan] MLBB processes: %zu", n);
    if (n == 0) {
        ui_log("[scan] none — open MLBB first (look for com.mobile.legends)");
        return;
    }
    for (size_t i = 0; i < n; ++i)
        ui_log("[scan] pid=%d  %s", (int)list[i].pid, list[i].cmdline);
}

static void reader_loop();
static void reader_start_once();

static void attach_pid(pid_t p, const char *why) {
    if (p <= 0) {
        ui_log("[%s] no pid", why);
        esp_reset();
        g_wantPid.store(0);
        return;
    }
    char cmd[280];
    proc_format_cmdline(p, cmd, sizeof(cmd));
    const bool probe = proc_probe_read(p);
    ui_log("[%s] pid=%d probe=%s", why, (int)p, probe ? "OK" : "FAIL");
    ui_log("  cmd: %s", cmd);
    if (!probe)
        ui_log("  process_vm_readv failed — need root or correct target process");
    esp_attach(p);
    g_wantPid.store(p);
    if (!esp_is_resolved()) g_needResolve.store(true);
}

static void reader_loop() {
    auto lastScan = std::chrono::steady_clock::now();
    EspStatus lastStatus = EspStatus::Idle;
    pid_t lastPid = 0;

    while (g_runReader.load()) {
        pid_t pid = g_wantPid.load();

        if (g_autoFind && pid <= 0) {
            pid_t p = proc_find_unity_kills();
            if (p <= 0 && g_fallbackPid)
                p = proc_find_mlbb_fallback();
            if (p > 0)
                attach_pid(p, "auto");
            else {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastScan > std::chrono::seconds(4)) {
                    lastScan = now;
                    ui_log("[auto] UnityKillsMe not found — tap Scan or open MLBB");
                    scan_and_log_processes();
                }
            }
            pid = g_wantPid.load();
        }

        if (pid > 0 && !proc_alive(pid)) {
            ui_log("[proc] pid %d died — reset", (int)pid);
            esp_reset();
            g_wantPid.store(0);
            g_needResolve.store(true);
            lastPid = 0;
        } else if (pid > 0) {
            if (g_needResolve.exchange(false)) {
                ui_log("[resolve] start pid=%d", (int)pid);
                EspResolveReport rep{};
                esp_resolve(&rep);
                log_resolve_result(rep);
            }
            EspFrame f;
            esp_tick(f);
            if (f.status != lastStatus || pid != lastPid) {
                ui_log("[tick] %s | %s", status_name(f.status), f.hint);
                if (f.status == EspStatus::Ready) {
                    ui_log("  bm=0x%llx lp=(%.1f,%.1f) dict=0x%llx/0x%llx",
                           (unsigned long long)f.bmInstance, f.selfX, f.selfZ,
                           (unsigned long long)f.playerDict, (unsigned long long)f.monsterDict);
                }
                lastStatus = f.status;
                lastPid = pid;
            }
            std::lock_guard<std::mutex> lock(g_frameMu);
            g_frame = std::move(f);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

static void draw_ui() {
    EspFrame fr;
    {
        std::lock_guard<std::mutex> lock(g_frameMu);
        fr = g_frame;
    }
    const EspResolveReport &rep = esp_last_resolve_report();
    /* W2S must use the same size as the GL overlay surface (ImGui framebuffer). */
    float sw = (float)g_width;
    float sh = (float)g_height;
    if (sw < 1.f && g_displayW > 0) sw = (float)g_displayW;
    if (sh < 1.f && g_displayH > 0) sh = (float)g_displayH;

    esp_draw_overlay(ImGui::GetBackgroundDrawList(), sw, sh, fr, g_feat);

    if (!g_showMenu) return;

    ImGui::SetNextWindowSize({360, 420}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("MLBB ESP", &g_showMenu, ImGuiWindowFlags_None)) {
        ImGui::TextDisabled("Build #87 base — external vm_readv + maps (no ptrace).");
        ImGui::Text("Status: %s", status_name(fr.status));
        if (fr.hint[0]) ImGui::TextWrapped("%s", fr.hint);

        if (ImGui::Button("Resolve")) {
            esp_request_force_resolve();
            g_needResolve.store(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Find pid")) {
            pid_t p = proc_find_unity_kills();
            if (p <= 0 && g_fallbackPid) p = proc_find_mlbb_fallback();
            attach_pid(p, "find");
        }

        ImGui::SeparatorText("Enemy");
        ImGui::Checkbox("Box", &g_feat.box);
        ImGui::SameLine();
        ImGui::Checkbox("Line", &g_feat.line);
        ImGui::SameLine();
        ImGui::Checkbox("Health", &g_feat.health);
        ImGui::Checkbox("Name", &g_feat.name);
        ImGui::SameLine();
        ImGui::Checkbox("Distance", &g_feat.distance);

        ImGui::SeparatorText("Monster");
        ImGui::Checkbox("Box##m", &g_feat.monsterBox);
        ImGui::SameLine();
        ImGui::Checkbox("Name##m", &g_feat.monsterName);
        ImGui::SameLine();
        ImGui::Checkbox("Health##m", &g_feat.monsterHealth);

        if (ImGui::CollapsingHeader("Advanced")) {
            ImGui::Checkbox("Auto find UnityKillsMe", &g_autoFind);
            ImGui::SameLine();
            ImGui::Checkbox("Fallback MLBB pid", &g_fallbackPid);
            const pid_t pid = g_wantPid.load();
            ImGui::Text("PID: %d", (int)pid);
            if (ImGui::Button("Scan MLBB")) scan_and_log_processes();
            ImGui::SameLine();
            if (ImGui::Button("Copy logs")) {
                static char logCopy[65536];
                ui_log_copy_all(logCopy, sizeof(logCopy));
                if (android_copy_to_clipboard(logCopy))
                    ui_log("[ui] logs copied");
            }
            if (pid > 0 && ImGui::Button("Scan IL2CPP")) log_il2cpp_scan(pid);
            ImGui::Text("IL2CPP: %s", rep.il2cppLib[0] ? rep.il2cppLib : "(none)");
            ImGui::TextWrapped("%s", rep.detail);
            ImGui::Checkbox("Log auto-scroll", &g_logScrollBottom);
            static char logText[65536];
            ui_log_copy_all(logText, sizeof(logText));
            ImGui::BeginChild("logs", {0, 140}, true);
            ImGui::TextUnformatted(logText);
            if (g_logScrollBottom) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }
    }
    ImGui::End();
}

static void apply_display_metrics() {
    if (g_width < 1) g_width = 1;
    if (g_height < 1) g_height = 1;
    if (initialScreenSize.x <= 0.f && g_displayW > 0)
        initialScreenSize = ImVec2((float)g_displayW, (float)g_displayH);
    else if (initialScreenSize.x <= 0.f)
        initialScreenSize = ImVec2((float)g_width, (float)g_height);
    if (initialScreenSize.x < 1.f) initialScreenSize.x = (float)g_width;
    if (initialScreenSize.y < 1.f) initialScreenSize.y = (float)g_height;
}

static bool init_imgui_backends() {
    if (!ImGui::GetCurrentContext()) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
        io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
        io.ConfigFlags &= ~ImGuiConfigFlags_DockingEnable;
        ImGui::StyleColorsDark();
        apply_menu_style();
        io.Fonts->AddFontDefault();
        ImGui::GetStyle().TouchExtraPadding = ImVec2(4.f, 4.f);
    }
    if (g_imguiBackend) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        g_imguiBackend = false;
    }
    ImGui_ImplAndroid_Init(nullptr);
    if (!ImGui_ImplOpenGL3_Init(g_glslVersion)) {
        __android_log_print(ANDROID_LOG_ERROR, "MlbbEsp", "ImGui_ImplOpenGL3_Init failed");
        return false;
    }
    g_imguiBackend = true;
    return true;
}

static void shutdown_imgui_only() {
    if (g_imguiBackend) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        g_imguiBackend = false;
    }
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    g_glReady = false;
}

static void render_imgui_frame() {
    if (!g_glReady || !g_imguiBackend) return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width, g_height);
    ImGui::NewFrame();
    draw_ui();
    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    if (!dd || dd->DisplaySize.x <= 0.f || dd->DisplaySize.y <= 0.f) return;
    glViewport(0, 0, g_width, g_height);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    ImGui_ImplOpenGL3_RenderDrawData(dd);
}

void ui_set_display_size(int width, int height) {
    if (width > 0 && height > 0) {
        g_displayW = width;
        g_displayH = height;
        if (initialScreenSize.x <= 0.f)
            initialScreenSize = ImVec2((float)width, (float)height);
    }
}

void ui_esp_pipeline_idle() {
    /* no-op: keep Build #12 auto-find / resolve behavior on overlay start */
}

void ui_gl_surface_created() {
    std::lock_guard<std::mutex> lock(g_glMu);
    shutdown_imgui_only();
    if (!init_imgui_backends()) return;
    g_glReady = true;
    reader_start_once();
    ui_log("MLBB ESP overlay — tap ≡ for menu, touches pass to game when closed");
}

static void reader_start_once() {
    if (g_readerStarted.exchange(true)) return;
    g_runReader.store(true);
    g_reader = std::thread(reader_loop);
}

void ui_gl_surface_changed(int width, int height) {
    std::lock_guard<std::mutex> lock(g_glMu);
    g_width = width > 0 ? width : 1;
    g_height = height > 0 ? height : 1;
    apply_display_metrics();
}

void ui_gl_surface_destroyed() {
    std::lock_guard<std::mutex> lock(g_glMu);
    g_runReader.store(false);
    if (g_reader.joinable()) g_reader.join();
    g_readerStarted.store(false);
    shutdown_imgui_only();
}

void ui_gl_draw_frame() {
    std::lock_guard<std::mutex> lock(g_glMu);
    render_imgui_frame();
}

void ui_touch(int action, float x, float y) {
    if (!g_glReady || !g_showMenu || !ImGui::GetCurrentContext()) return;
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        io.AddMouseButtonEvent(0, true);
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
        io.AddMouseButtonEvent(0, false);
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        break;
    default:
        break;
    }
}

void ui_open_menu() { g_showMenu = true; }

void ui_toggle_menu() { g_showMenu = !g_showMenu; }

bool ui_menu_interactive() { return g_showMenu; }
