#pragma once
#include <sys/types.h>

struct EspDeviceIdSnap {
    bool refreshed = false;
    char gameServerJsonId[192] = {};
    char gameServerReportId[192] = {};
    char gameServerUniqueId[192] = {};
    char gameServerAndroidId[192] = {};
    char gameServerAdId[192] = {};
    char sdkDeviceId[192] = {};
    char sdkUniqueId[192] = {};
    char sdkAndroidId[192] = {};
    char sdkAdvertisingId[192] = {};
    char sdkOaid[192] = {};
    char sdkChannel[96] = {};
    char utilAndroidId[192] = {};
    char utilAdId[192] = {};
    char utilTelephonyId[192] = {};
    char utilMac[96] = {};
    char hint[128] = {};
};

void esp_device_id_on_resolve(pid_t pid, uintptr_t gameServerConfigKlass, uintptr_t sdkCommonKlass,
                              uintptr_t deviceUtilKlass);
void esp_device_id_clear();
void esp_device_id_refresh(pid_t pid);
const EspDeviceIdSnap &esp_device_id_snapshot();
void esp_device_id_format_copy_all(char *out, size_t outSz);
void esp_device_id_draw_panel(pid_t pid);