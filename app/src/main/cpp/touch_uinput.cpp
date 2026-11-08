#include "touch_uinput.hpp"

#include <android/log.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

static int g_uinputFd = -1;
static int g_absMaxX = 0;
static int g_absMaxY = 0;
static int g_screenW = 0;
static int g_screenH = 0;
static bool g_loggedOpen = false;

static bool is_touch_device(int fd) {
    uint8_t ev_bits[EV_MAX / 8 + 1] = {};
    uint8_t abs_bits[ABS_MAX / 8 + 1] = {};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) return false;
    if (!(ev_bits[EV_ABS / 8] & (1 << (EV_ABS % 8)))) return false;
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) return false;
    return (abs_bits[ABS_MT_POSITION_X / 8] & (1 << (ABS_MT_POSITION_X % 8)))
        || (abs_bits[ABS_X / 8] & (1 << (ABS_X % 8)));
}

static bool probe_abs_max(int &maxX, int &maxY) {
    for (int i = 0; i < 32; ++i) {
        char path[64];
        std::snprintf(path, sizeof(path), "/dev/input/event%d", i);
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        if (!is_touch_device(fd)) {
            close(fd);
            continue;
        }
        input_absinfo ax{}, ay{};
        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) == 0
            && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) == 0 && ax.maximum > 0
            && ay.maximum > 0) {
            maxX = ax.maximum;
            maxY = ay.maximum;
            close(fd);
            return true;
        }
        close(fd);
    }
    return false;
}

static void write_ev(int fd, int type, int code, int value) {
    input_event ev{};
    gettimeofday(&ev.time, nullptr);
    ev.type = (uint16_t)type;
    ev.code = (uint16_t)code;
    ev.value = value;
    (void)write(fd, &ev, sizeof(ev));
}

static int to_abs_x(int px) {
    if (g_absMaxX <= 0 || g_screenW <= 0) return px;
    return (int)((int64_t)px * g_absMaxX / g_screenW);
}

static int to_abs_y(int py) {
    if (g_absMaxY <= 0 || g_screenH <= 0) return py;
    return (int)((int64_t)py * g_absMaxY / g_screenH);
}

static bool shell_tap(int x, int y, const char *fmt) {
    char cmd[160];
    std::snprintf(cmd, sizeof(cmd), fmt, x, y);
    return std::system(cmd) == 0;
}

} // namespace

bool touch_uinput_ready() { return g_uinputFd >= 0; }

bool touch_uinput_open(int screenW, int screenH) {
    if (g_uinputFd >= 0) return true;
    if (screenW < 1 || screenH < 1) return false;
    g_screenW = screenW;
    g_screenH = screenH;
    if (!probe_abs_max(g_absMaxX, g_absMaxY)) {
        g_absMaxX = screenW;
        g_absMaxY = screenH;
    }

    const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;

    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_FINGER);

    uinput_user_dev dev{};
    std::snprintf(dev.name, UINPUT_MAX_NAME_SIZE, "xorel-retri-vtouch");
    dev.id.bustype = BUS_VIRTUAL;
    dev.absmin[ABS_MT_POSITION_X] = 0;
    dev.absmax[ABS_MT_POSITION_X] = g_absMaxX;
    dev.absmin[ABS_MT_POSITION_Y] = 0;
    dev.absmax[ABS_MT_POSITION_Y] = g_absMaxY;
    dev.absmin[ABS_MT_TRACKING_ID] = 0;
    dev.absmax[ABS_MT_TRACKING_ID] = 65535;
    if (write(fd, &dev, sizeof(dev)) != (ssize_t)sizeof(dev)) {
        close(fd);
        return false;
    }
    if (ioctl(fd, UI_DEV_CREATE) != 0) {
        close(fd);
        return false;
    }

    g_uinputFd = fd;
    if (!g_loggedOpen) {
        g_loggedOpen = true;
        __android_log_print(ANDROID_LOG_INFO, "MlbbEsp", "[retri] uinput open max=%dx%d scr=%dx%d",
                            g_absMaxX, g_absMaxY, screenW, screenH);
    }
    return true;
}

void touch_uinput_close() {
    if (g_uinputFd >= 0) {
        ioctl(g_uinputFd, UI_DEV_DESTROY);
        close(g_uinputFd);
        g_uinputFd = -1;
    }
}

bool touch_uinput_tap(int x, int y) {
    if (g_uinputFd < 0) return false;
    const int ax = to_abs_x(x);
    const int ay = to_abs_y(y);

    write_ev(g_uinputFd, EV_ABS, ABS_MT_TRACKING_ID, 52020);
    write_ev(g_uinputFd, EV_ABS, ABS_MT_POSITION_X, ax);
    write_ev(g_uinputFd, EV_ABS, ABS_MT_POSITION_Y, ay);
    write_ev(g_uinputFd, EV_KEY, BTN_TOUCH, 1);
    write_ev(g_uinputFd, EV_KEY, BTN_TOOL_FINGER, 1);
    write_ev(g_uinputFd, EV_SYN, SYN_REPORT, 0);

    write_ev(g_uinputFd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    write_ev(g_uinputFd, EV_KEY, BTN_TOUCH, 0);
    write_ev(g_uinputFd, EV_KEY, BTN_TOOL_FINGER, 0);
    write_ev(g_uinputFd, EV_SYN, SYN_REPORT, 0);
    return true;
}

const char *touch_inject_screen_tap(int x, int y, int screenW, int screenH) {
    static char s_method[24];
    if (touch_uinput_open(screenW, screenH) && touch_uinput_tap(x, y)) {
        std::snprintf(s_method, sizeof(s_method), "uinput");
        return s_method;
    }
    std::snprintf(s_method, sizeof(s_method), "fail");
    return s_method;
}

const char *touch_inject_screen_swipe(int x1, int y1, int x2, int y2, int durationMs, int screenW,
                                      int screenH) {
    static char s_method[24];
    (void)touch_uinput_open(screenW, screenH);
    static const char *kSu[] = {
        "su -c 'input swipe %d %d %d %d %d' 2>/dev/null",
        "su -c \"input swipe %d %d %d %d %d\" 2>/dev/null",
        "su 0 sh -c 'input swipe %d %d %d %d %d' 2>/dev/null",
    };
    char cmd[192];
    for (const char *fmt : kSu) {
        std::snprintf(cmd, sizeof(cmd), fmt, x1, y1, x2, y2, durationMs);
        if (std::system(cmd) == 0) {
            std::snprintf(s_method, sizeof(s_method), "su-swipe");
            return s_method;
        }
    }
    std::snprintf(cmd, sizeof(cmd), "input swipe %d %d %d %d %d 2>/dev/null", x1, y1, x2, y2,
                  durationMs);
    if (std::system(cmd) == 0) {
        std::snprintf(s_method, sizeof(s_method), "input-swipe");
        return s_method;
    }
    std::snprintf(s_method, sizeof(s_method), "fail");
    return s_method;
}
