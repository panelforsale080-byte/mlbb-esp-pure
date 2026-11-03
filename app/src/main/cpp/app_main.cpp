#include "esp_engine.hpp"
#include "proc.hpp"

#include <android/log.h>
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

static bool g_autoFind = true;
static bool g_showEsp = true;
static bool g_showPlayers = true;
static bool g_showMonsters = true;
static bool g_needResolve = true;

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
        EGL_DEPTH_SIZE, 24,
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

static void reader_loop() {
    while (g_runReader.load()) {
        pid_t pid = g_wantPid.load();
        if (g_autoFind && pid <= 0) {
            pid = proc_find_unity_kills();
            if (pid > 0) {
                esp_attach(pid);
                g_wantPid.store(pid);
                g_needResolve = true;
            }
        }
        if (pid > 0 && !proc_alive(pid)) {
            esp_reset();
            g_wantPid.store(0);
            g_needResolve = true;
        } else if (pid > 0) {
            if (g_needResolve) {
                esp_resolve();
                g_needResolve = false;
            }
            EspFrame f;
            esp_tick(f);
            std::lock_guard<std::mutex> lock(g_frameMu);
            g_frame = std::move(f);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
}

static void draw_radar(const EspFrame &fr) {
    if (!g_showEsp) return;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    const ImVec2 center(g_width * 0.78f, g_height * 0.5f);
    const float radius = std::min(g_width, g_height) * 0.32f;
    const float scale = radius / 40.f; /* ~40m ring */

    dl->AddCircle(center, radius, IM_COL32(60, 60, 60, 180), 64, 2.f);
    dl->AddCircle(center, radius * 0.5f, IM_COL32(40, 40, 40, 120), 48, 1.f);
    dl->AddLine({center.x - radius, center.y}, {center.x + radius, center.y},
                IM_COL32(50, 50, 50, 100));
    dl->AddLine({center.x, center.y - radius}, {center.x, center.y + radius},
                IM_COL32(50, 50, 50, 100));
    dl->AddCircleFilled(center, 6.f, IM_COL32(80, 200, 255, 255));

    for (const EspTarget &t : fr.targets) {
        if (t.kind == 0 && !g_showPlayers) continue;
        if (t.kind == 1 && !g_showMonsters) continue;
        const float rx = t.relX * scale;
        const float rz = t.relZ * scale;
        if (rx * rx + rz * rz > radius * radius) continue;
        const ImVec2 p(center.x + rx, center.y + rz);
        const ImU32 col = t.kind ? IM_COL32(255, 180, 60, 255) : IM_COL32(255, 70, 70, 255);
        dl->AddCircleFilled(p, 5.f, col);
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%s %dm", t.label, t.meters);
        dl->AddText({p.x + 8.f, p.y - 8.f}, IM_COL32(240, 240, 240, 230), buf);
    }
}

static void draw_ui() {
    EspFrame fr;
    {
        std::lock_guard<std::mutex> lock(g_frameMu);
        fr = g_frame;
    }

    ImGui::SetNextWindowSize({420, 0}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("MLBB ESP (pure C++)", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Checkbox("Auto find UnityKillsMe", &g_autoFind);
        ImGui::Checkbox("ESP", &g_showEsp);
        ImGui::Checkbox("Players", &g_showPlayers);
        ImGui::Checkbox("Monsters", &g_showMonsters);

        pid_t pid = g_wantPid.load();
        ImGui::Text("PID: %d", (int)pid);
        ImGui::Text("Status: %s", fr.hint);

        if (ImGui::Button("Find process")) {
            const pid_t p = proc_find_unity_kills();
            if (p > 0 && proc_probe_read(p)) {
                esp_attach(p);
                g_wantPid.store(p);
                g_needResolve = true;
            } else if (p > 0) {
                esp_attach(p);
                g_wantPid.store(p);
                g_needResolve = true;
            } else {
                esp_reset();
                g_wantPid.store(0);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Resolve")) g_needResolve = true;

        if (ImGui::CollapsingHeader("Targets")) {
            ImGui::BeginChild("tgt", {0, 180}, true);
            for (const EspTarget &t : fr.targets) {
                ImGui::Text("%s  %dm  HP %d/%d", t.label, t.meters, t.hp, t.hpMax);
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
    draw_radar(fr);
}

static void handle_cmd(android_app *app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (app->window && g_eglDisplay == EGL_NO_DISPLAY) {
            init_egl(app->window);
            ImGui_ImplAndroid_Init(app->window);
            ImGui_ImplOpenGL3_Init("#version 300 es");
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
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

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
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplAndroid_NewFrame(g_width, g_height);
        ImGui::NewFrame();

        draw_ui();

        ImGui::Render();
        glViewport(0, 0, g_width, g_height);
        glClearColor(0.08f, 0.08f, 0.1f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(g_eglDisplay, g_eglSurface);
    }
}
