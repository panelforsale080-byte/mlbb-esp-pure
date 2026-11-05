/* Injected into MLBB: build #49 Camera.WorldToScreenPoint (in-process, no overlay ptrace). */
#include <android/log.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>

#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, "mlbb_espw2s", __VA_ARGS__)

struct Vec3 {
    float x, y, z;
};

using CamGetMainFn = void *(*)();
using CamW2sFn = Vec3 (*)(void *, Vec3);

static constexpr uint32_t kMagic = 0x48533257u; /* W2SH */
static constexpr int kMaxPts = 128;

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

static EspW2sShm *g_shm = nullptr;
static CamGetMainFn g_getMain = nullptr;
static CamW2sFn g_w2s = nullptr;
static void *g_cam = nullptr;
static float g_sw = 0.f, g_sh = 0.f;

static bool read_cfg(pid_t pid) {
    char path[128];
    std::snprintf(path, sizeof(path), "/data/local/tmp/mlbb_esp_%d.cfg", (int)pid);
    FILE *f = std::fopen(path, "r");
    if (!f) return false;
    uintptr_t gm = 0, w2s = 0;
    float sw = 0.f, sh = 0.f;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::sscanf(line, "get_main=%llx", (unsigned long long *)&gm) == 1) continue;
        if (std::sscanf(line, "w2s=%llx", (unsigned long long *)&w2s) == 1) continue;
        if (std::sscanf(line, "screen_w=%f", &sw) == 1) continue;
        if (std::sscanf(line, "screen_h=%f", &sh) == 1) continue;
    }
    std::fclose(f);
    if (!gm || !w2s) return false;
    g_getMain = reinterpret_cast<CamGetMainFn>(gm);
    g_w2s = reinterpret_cast<CamW2sFn>(w2s);
    g_sw = sw;
    g_sh = sh;
    return true;
}

static void write_shm_addr(pid_t pid, uintptr_t addr) {
    char path[128];
    std::snprintf(path, sizeof(path), "/data/local/tmp/mlbb_esp_%d.shm", (int)pid);
    FILE *f = std::fopen(path, "w");
    if (!f) return;
    std::fprintf(f, "0x%llx\n", (unsigned long long)addr);
    std::fclose(f);
}

static bool w2s_one(float wx, float wy, float wz, float &sx, float &sy) {
    if (!g_getMain || !g_w2s) return false;
    if (!g_cam) g_cam = g_getMain();
    if (!g_cam) return false;
    const Vec3 in{wx, wy, wz};
    const Vec3 out = g_w2s(g_cam, in);
    const float sw = g_shm && g_shm->screen_w > 1.f ? g_shm->screen_w : g_sw;
    const float sh = g_shm && g_shm->screen_h > 1.f ? g_shm->screen_h : g_sh;
    if (sw < 1.f || sh < 1.f) return false;
    if (out.z > 0.f) {
        sx = out.x;
        sy = sh - out.y;
    } else {
        sx = sw - out.x;
        sy = out.y;
    }
    return std::isfinite(sx) && std::isfinite(sy);
}

static void *worker(void *) {
    uint32_t last = 0;
    while (g_shm) {
        const uint32_t seq = g_shm->req_seq;
        if (seq != last && seq != g_shm->ack_seq) {
            const uint32_t n = g_shm->count;
            const uint32_t lim = n > kMaxPts ? kMaxPts : n;
            for (uint32_t i = 0; i < lim; ++i) {
                float sx = 0.f, sy = 0.f;
                if (w2s_one(g_shm->wx[i], g_shm->wy[i], g_shm->wz[i], sx, sy)) {
                    g_shm->sx[i] = sx;
                    g_shm->sy[i] = sy;
                    g_shm->ok[i] = 1;
                } else {
                    g_shm->ok[i] = 0;
                }
            }
            __sync_synchronize();
            g_shm->ack_seq = seq;
            last = seq;
        }
        usleep(1500);
    }
    return nullptr;
}

__attribute__((constructor)) static void on_load() {
    const pid_t pid = getpid();
    if (!read_cfg(pid)) {
        ALOG("no cfg for pid %d", (int)pid);
        return;
    }
    g_shm = static_cast<EspW2sShm *>(
        mmap(nullptr, sizeof(EspW2sShm), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (g_shm == MAP_FAILED) {
        ALOG("mmap shm failed");
        g_shm = nullptr;
        return;
    }
    std::memset(g_shm, 0, sizeof(EspW2sShm));
    g_shm->magic = kMagic;
    g_shm->version = 1;
    g_shm->screen_w = g_sw;
    g_shm->screen_h = g_sh;
    write_shm_addr(pid, reinterpret_cast<uintptr_t>(g_shm));
    pthread_t th;
    pthread_create(&th, nullptr, worker, nullptr);
    pthread_detach(th);
    ALOG("ready pid=%d get_main=%p w2s=%p", (int)pid, (void *)g_getMain, (void *)g_w2s);
}
