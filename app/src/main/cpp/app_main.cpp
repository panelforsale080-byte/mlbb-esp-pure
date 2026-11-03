#include "esp_engine.hpp"
#include "esp_draw.hpp"
#include "proc.hpp"
#include "ui_log.hpp"
#include "clip_android.hpp"
#include "remote_il2cpp.hpp"

#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"

static EGLDisplay g_eglDisplay = EGL_NO_DISPLAY;
static EGLSurface g_eglSurface = EGL_NO_SURFACE;
static EGLContext g_eglContext = EGL_NO_CONTEXT;
static int g_width = 0, g_height = 0;
ImVec2 initialScreenSize;

static std::atomic<bool> g_runReader{true};
static std::mutex g_frameMu;
static EspFrame g_frame;
static std::atomic<pid_t> g_wantPid{0};
static std::atomic<bool> g_needResolve{true};

static bool g_autoFind = true;
static bool g_fallbackPid = true;
static bool g_showMenu = true;
static bool g_logScrollBottom = true;
static EspFeatures g_feat{};
static android_app *g_app = nullptr;

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

static bool init_egl(ANativeWindow *window) {
    g_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_eglDisplay == EGL_NO_DISPLAY) return false;
    if (!eglInitialize(g_eglDisplay, nullptr, nullptr)) return false;

    const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint num = 0;
    if (!eglChooseConfig(g_eglDisplay, attribs, &config, 1, &num) || num == 0)
        return false;

    const EGLint ctxAtt[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    g_eglContext = eglCreateContext(g_eglDisplay, config, EGL_NO_CONTEXT, ctxAtt);
    g_eglSurface = eglCreateWindowSurface(g_eglDisplay, config, window, nullptr);
    if (g_eglContext == EGL_NO_CONTEXT || g_eglSurface == EGL_NO_SURFACE) return false;
    if (!eglMakeCurrent(g_eglDisplay, g_eglSurface, g_eglSurface, g_eglContext))
        return false;
    return true;
}

static void shutdown_egl() {
    if (g_eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_eglContext != EGL_NO_CONTEXT) eglDestroyContext(g_eglDisplay, g_eglContext);
        if (g_eglSurface != EGL_NO_SURFACE) eglDestroySurface(g_eglDisplay, g_eglSurface);
        eglTerminate(g_eglDisplay);
    }
    g_eglDisplay = EGL_NO_DISPLAY;
    g_eglContext = EGL_NO_CONTEXT;
    g_eglSurface = EGL_NO_SURFACE;
}

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
    g_needResolve.store(true);
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

        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
}

static void draw_ui() {
    EspFrame fr;
    {
        std::lock_guard<std::mutex> lock(g_frameMu);
        fr = g_frame;
    }
    const EspResolveReport &rep = esp_last_resolve_report();
    const float sw = (float)g_width;
    const float sh = (float)g_height;

    esp_draw_overlay(ImGui::GetBackgroundDrawList(), sw, sh, fr, g_feat);

    if (!g_showMenu) {
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::SetNextWindowPos({12.f, 12.f}, ImGuiCond_Always);
        if (ImGui::Begin("##esp_fab", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoMove)) {
            if (ImGui::Button("Menu")) g_showMenu = true;
            ImGui::SameLine();
            ImGui::TextDisabled("%s", status_name(fr.status));
        }
        ImGui::End();
        return;
    }

    ImGui::SetNextWindowSize({360, 420}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("MLBB ESP", &g_showMenu, ImGuiWindowFlags_None)) {
        ImGui::TextDisabled("Overlay — open MLBB, allow display over apps, enter match.");
        ImGui::Text("Status: %s", status_name(fr.status));
        if (fr.hint[0]) ImGui::TextWrapped("%s", fr.hint);

        if (ImGui::Button("Resolve")) g_needResolve.store(true);
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
                if (android_copy_to_clipboard(g_app, logCopy))
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

static void handle_cmd(android_app *app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (app->window && g_eglDisplay == EGL_NO_DISPLAY) {
            init_egl(app->window);
            ImGui_ImplAndroid_Init(app->window);
            ImGui_ImplOpenGL3_Init("#version 300 es");
            ui_log("MLBB ESP overlay — allow over other apps, MLBB match, Resolve");
        }
        break;
    case APP_CMD_TERM_WINDOW:
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        shutdown_egl();
        break;
    case APP_CMD_WINDOW_RESIZED:
        if (app->window) {
            g_width = ANativeWindow_getWidth(app->window);
            g_height = ANativeWindow_getHeight(app->window);
            if (initialScreenSize.x <= 0.f || initialScreenSize.y <= 0.f)
                initialScreenSize = ImVec2((float)g_width, (float)g_height);
        }
        break;
    default:
        break;
    }
}

static int32_t handle_input(android_app *app, AInputEvent *event) {
    return ImGui_ImplAndroid_HandleInputEvent(event);
}

void android_main(android_app *app) {
    g_app = app;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    apply_menu_style();

    std::thread reader(reader_loop);

    while (true) {
        int events = 0;
        android_poll_source *source = nullptr;
        while (ALooper_pollOnce(g_eglDisplay == EGL_NO_DISPLAY ? 0 : 0, nullptr, &events,
                                (void **)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) {
                g_runReader.store(false);
                reader.join();
                return;
            }
        }

        if (g_eglDisplay == EGL_NO_DISPLAY) continue;

        if (g_width <= 0 || g_height <= 0) {
            g_width = ANativeWindow_getWidth(app->window);
            g_height = ANativeWindow_getHeight(app->window);
            if (initialScreenSize.x <= 0.f || initialScreenSize.y <= 0.f)
                initialScreenSize = ImVec2((float)g_width, (float)g_height);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplAndroid_NewFrame(g_width, g_height);
        ImGui::NewFrame();

        draw_ui();

        ImGui::Render();
        glViewport(0, 0, g_width, g_height);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(g_eglDisplay, g_eglSurface);
    }
}
