#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <cmath>
#include <linux/input.h>
#include <linux/uinput.h>
#include <time.h>
#include <string.h>

#include "imgui.h"

#include <mutex>

#define maxE 5
#define maxF 10
#define UNGRAB 0
#define GRAB 1

// Pengaturan radius blokir (dalam pixel)
static float blockRadius = 130.0f;
bool isBeingBlocked = false;
bool other_touch;

static std::mutex touch_mutex;

static uint32_t orientation = 0;
static float screenHeight = 0, screenWidth = 0;

struct touchObj {
    bool isDown = false;
    bool isCaptured = false;
    int x = 0;
    int y = 0;
    int id = 0;
};

struct targ {
    int fdNum;
    float S2TX;
    float S2TY;
};

static struct {
    input_event downEvent[2]{{{}, EV_KEY, BTN_TOUCH,       1},
                             {{}, EV_KEY, BTN_TOOL_FINGER, 1}};
    input_event event[512]{0};
} input;

static targ targF[maxE];
static touchObj Finger[maxE][maxF];
static int fdNum = 0, origfd[maxE], nowfd;
static float scale_x, scale_y;
static bool Touch_initialized = false;
static bool Touch_readOnly = false;

static bool checkDeviceIsTouch(int fd) {
    uint8_t ev_bits[EV_MAX / 8 + 1] = {0};
    uint8_t abs_bits[ABS_MAX / 8 + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) return false;
    if (!(ev_bits[EV_ABS / 8] & (1 << (EV_ABS % 8)))) return false;
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
    return (abs_bits[ABS_MT_POSITION_X / 8] & (1 << (ABS_MT_POSITION_X % 8))) ||
           (abs_bits[ABS_X / 8] & (1 << (ABS_X % 8)));
}

static void genRandomString(char *string, int length) {
    int flag, i;
    srand((unsigned) time(NULL) + length);
    for (i = 0; i < length - 1; i++) {
        flag = rand() % 3;
        switch (flag) {
            case 0: string[i] = 'A' + rand() % 26; break;
            case 1: string[i] = 'a' + rand() % 26; break;
            case 2: string[i] = '0' + rand() % 10; break;
            default: string[i] = 'x'; break;
        }
    }
    string[length - 1] = '\0';
}

static void Upload() {
    std::lock_guard<std::mutex> lock(touch_mutex);
    static bool isFirstDown = true;
    int tmpCnt = 0, tmpCnt2 = 0, i, j;
    for (i = 0; i < fdNum; i++) {
        for (j = 0; j < maxF; j++) {
            if (Finger[i][j].isDown && !Finger[i][j].isCaptured) {
                if (tmpCnt2++ > 10) goto finish;

                input.event[tmpCnt].type = EV_ABS;
                input.event[tmpCnt].code = ABS_MT_POSITION_X;
                input.event[tmpCnt].value = Finger[i][j].x;
                tmpCnt++;

                input.event[tmpCnt].type = EV_ABS;
                input.event[tmpCnt].code = ABS_MT_POSITION_Y;
                input.event[tmpCnt].value = Finger[i][j].y;
                tmpCnt++;

                input.event[tmpCnt].type = EV_ABS;
                input.event[tmpCnt].code = ABS_MT_TRACKING_ID;
                input.event[tmpCnt].value = Finger[i][j].id;
                tmpCnt++;

                input.event[tmpCnt].type = EV_SYN;
                input.event[tmpCnt].code = SYN_MT_REPORT;
                input.event[tmpCnt].value = 0;
                tmpCnt++;
            }
        }
    }
    finish:
    bool is = false;
    if (tmpCnt == 0) {
        input.event[tmpCnt].type = EV_SYN;
        input.event[tmpCnt].code = SYN_MT_REPORT;
        input.event[tmpCnt].value = 0;
        tmpCnt++;
        if (!isFirstDown) {
            isFirstDown = true;
            input.event[tmpCnt].type = EV_KEY;
            input.event[tmpCnt].code = BTN_TOUCH;
            input.event[tmpCnt].value = 0;
            tmpCnt++;
            input.event[tmpCnt].type = EV_KEY;
            input.event[tmpCnt].code = BTN_TOOL_FINGER;
            input.event[tmpCnt].value = 0;
            tmpCnt++;
        }
    } else {
        is = true;
    }
    input.event[tmpCnt].type = EV_SYN;
    input.event[tmpCnt].code = SYN_REPORT;
    input.event[tmpCnt].value = 0;
    tmpCnt++;

    if (is && isFirstDown) {
        isFirstDown = false;
        write(nowfd, &input, sizeof(struct input_event) * (tmpCnt + 2));
    } else {
        write(nowfd, input.event, sizeof(struct input_event) * tmpCnt);
    }
}

static void *TypeA(void *arg) {
    targ tmp = *(targ *) arg;
    int i = tmp.fdNum;
    float S2TX = tmp.S2TX;
    float S2TY = tmp.S2TY;
    int latest = 0;
    input_event inputEvent[64]{0};

    while (Touch_initialized) {
        auto readSize = (int32_t) read(origfd[i], inputEvent, sizeof(inputEvent));
        if (readSize <= 0 || (readSize % sizeof(input_event)) != 0) continue;

        size_t count = size_t(readSize) / sizeof(input_event);
        for (size_t j = 0; j < count; j++) {
            input_event &ie = inputEvent[j];
            if (ie.type == EV_ABS) {
                if (ie.code == ABS_MT_SLOT) {
                    latest = ie.value;
                } else if (ie.code == ABS_MT_TRACKING_ID) {
                    if (ie.value == -1) {
                        Finger[i][latest].isDown = false;
                        Finger[i][latest].isCaptured = false;
                    } else {
                        Finger[i][latest].id = (i * 2 + 1) * maxF + latest;
                        Finger[i][latest].isDown = true;
                    }
                } else if (ie.code == ABS_MT_POSITION_X || ie.code == ABS_X) {
                    Finger[i][latest].id = (i * 2 + 1) * maxF + latest;
                    Finger[i][latest].x = (int) (ie.value * S2TX);
                } else if (ie.code == ABS_MT_POSITION_Y || ie.code == ABS_Y) {
                    Finger[i][latest].id = (i * 2 + 1) * maxF + latest;
                    Finger[i][latest].y = (int) (ie.value * S2TY);
                }
            } else if (ie.type == EV_KEY) {
                if (ie.code == BTN_TOUCH || ie.code == BTN_LEFT) {
                    Finger[i][latest].isDown = (ie.value != 0);
                    if (!Finger[i][latest].isDown) Finger[i][latest].isCaptured = false;
                }
            }

            if (ie.code == SYN_REPORT) {
                ImGuiIO &io = ImGui::GetIO();
                if (Finger[i][latest].isDown) {
                    float xt = (float)Finger[i][latest].x / scale_x;
                    float yt = (float)Finger[i][latest].y / scale_y;
                    float fx, fy;

                    if (other_touch) {
                        if (orientation == 1) { fx = xt; fy = yt; }
                        else if (orientation == 2) { fy = yt; fx = screenHeight - xt; }
                        else if (orientation == 3) { fx = screenHeight - xt; fy = screenWidth - yt; }
                        else { fy = xt; fx = screenHeight - yt; }
                    } else {
                        if (orientation == 1) { fx = yt; fy = screenHeight - xt; }
                        else if (orientation == 2) { fx = screenHeight - xt; fy = screenWidth - yt; }
                        else if (orientation == 3) { fy = xt; fx = screenWidth - yt; }
                        else { fx = xt; fy = yt; }
                    }

                    // Gunakan io.MousePos untuk mendapatkan posisi mouse terkini
                    io.MousePos = {fx, fy};
                    io.MouseDown[0] = true;

                    // --- LOGIKA BLOKIR MENGGUNAKAN io.MousePos ---
                    if (Finger[0][9].isDown) {
                        // Ambil posisi bot saat ini (skala layar)
                        float botX = (float)Finger[0][9].x / scale_x;
                        float botY = (float)Finger[0][9].y / scale_y;

                        // Hitung jarak antara Mouse asli dengan Jari Bot
                        float dx = io.MousePos.x - botX;
                        float dy = io.MousePos.y - botY;
                        float dist = std::sqrt(dx * dx + dy * dy);

                        if (dist < blockRadius) {
                            isBeingBlocked = true;
                            Finger[0][9].isDown = false; // Lepas bot jika terlalu dekat
                            Upload();
                        } else {
                            isBeingBlocked = false;
                        }
                    }
                    // ---------------------------------------------

                    if (!Touch_readOnly) {
                        if (io.WantCaptureMouse) {
                            Finger[i][latest].isCaptured = true;
                        }
                        Upload();
                    }
                } else {
                    io.MouseDown[0] = false;
                    isBeingBlocked = false;
                    if (!Touch_readOnly) Upload();
                }
            }
        }
    }
    return nullptr;
}

bool Touch_Init(int w, int h, uint32_t orientation_, bool readOnly) {
    char temp[128], name[256];
    DIR *dir = opendir("/dev/input/");
    dirent *ptr = NULL;
    int eventCount = 0;
    while ((ptr = readdir(dir)) != NULL) {
        if (strstr(ptr->d_name, "event")) eventCount++;
    }
    closedir(dir);

    struct input_absinfo absX[maxE], absY[maxE];
    int screenX, screenY, minCnt = eventCount + 1;
    fdNum = 0;
    for (int i = 0; i <= eventCount; i++) {
        sprintf(temp, "/dev/input/event%d", i);
        int fd = open(temp, O_RDWR);
        if (fd < 0) continue;

        if (checkDeviceIsTouch(fd)) {
            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &absX[fdNum]) == 0 &&
                ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &absY[fdNum]) == 0) {

                origfd[fdNum] = fd;
                ioctl(fd, EVIOCGNAME(sizeof(name)), name);

                if (!readOnly && !strstr(name, "Virtual") && !strstr(name, "uinput")) {
                    ioctl(fd, EVIOCGRAB, GRAB);
                }

                if (i < minCnt) {
                    screenX = absX[fdNum].maximum;
                    screenY = absY[fdNum].maximum;
                    minCnt = i;
                }
                fdNum++;
                if (fdNum >= maxE) break;
            }
        } else close(fd);
    }

    if (fdNum == 0) return false;

    if (!readOnly) {
        nowfd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        struct uinput_user_dev ui_dev;
        memset(&ui_dev, 0, sizeof(ui_dev));
        genRandomString(ui_dev.name, 10);
        ui_dev.id.bustype = BUS_USB;
        ioctl(nowfd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
        ioctl(nowfd, UI_SET_EVBIT, EV_ABS);
        ioctl(nowfd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
        ioctl(nowfd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
        ioctl(nowfd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
        ioctl(nowfd, UI_SET_EVBIT, EV_SYN);
        ioctl(nowfd, UI_SET_EVBIT, EV_KEY);
        ioctl(nowfd, UI_SET_KEYBIT, BTN_TOUCH);
        ui_dev.absmax[ABS_MT_POSITION_X] = screenX;
        ui_dev.absmax[ABS_MT_POSITION_Y] = screenY;
        ui_dev.absmax[ABS_MT_TRACKING_ID] = 65535;
        write(nowfd, &ui_dev, sizeof(ui_dev));
        ioctl(nowfd, UI_DEV_CREATE);
    }

    Touch_initialized = true;
    Touch_readOnly = readOnly;
    ::screenWidth = w; ::screenHeight = h; ::orientation = orientation_;
    ::scale_x = (float) screenX / (orientation_ % 2 == 0 ? w : h);
    ::scale_y = (float) screenY / (orientation_ % 2 == 0 ? h : w);

    for (int i = 0; i < fdNum; i++) {
        targF[i].fdNum = i;
        targF[i].S2TX = (float) screenX / (float) absX[i].maximum;
        targF[i].S2TY = (float) screenY / (float) absY[i].maximum;
        pthread_t t;
        pthread_create(&t, NULL, TypeA, &targF[i]);
    }
    // system("chmod 000 -R /proc/bus/input/*");
    return true;
}

void Touch_Close() {
    if (Touch_initialized) {
        for (int i = 0; i < maxE; ++i) {
            if (origfd[i] > 0) {
                ioctl(origfd[i], EVIOCGRAB, UNGRAB);
                close(origfd[i]);
                origfd[i] = 0;
            }
        }
        if (nowfd > 0) { ioctl(nowfd, UI_DEV_DESTROY); close(nowfd); nowfd = 0; }
        Touch_initialized = false;
    }
}

void Touch_Down(float xt, float yt) {
    if (isBeingBlocked) return;

    float x, y;
    switch (orientation) {
        case 1: x = screenHeight - yt; y = xt; break;
        case 2: x = screenWidth - xt; y = screenHeight - yt; break;
        case 3: x = yt; y = screenWidth - xt; break;
        default: x = xt; y = yt; break;
    }
    Finger[0][9].id = 19;
    Finger[0][9].x = (int) (x * ::scale_x);
    Finger[0][9].y = (int) (y * ::scale_y);
    Finger[0][9].isDown = true;
    Upload();
}

void Touch_Move(float x, float y) {
    Touch_Down(x, y);
}

void Touch_Up() {
    Finger[0][9].isDown = false;
    isBeingBlocked = false;
    Upload();
}

void UpdateScreenData(int w, int h, uint32_t o) {
    ::screenWidth = w; ::screenHeight = h; ::orientation = o;
}
