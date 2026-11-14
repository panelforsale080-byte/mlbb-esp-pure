#pragma once
#include <sys/types.h>

struct EspDeviceIdSnap {
    bool refreshed = false;
    char gameServerJsonId[160] = {};
    char gameServerReportId[160] = {};
    char gameServerAndroidId[160] = {};
    char gameServerAdId[160] = {};
    char sdkDeviceId[160] = {};
    char sdkAndroidId[160] = {};
    char sdkAdvertisingId[160] = {};
    char hint[128] = {};
};

void esp_device_id_on_resolve(pid_t pid, uintptr_t gameServerConfigKlass, uintptr_t sdkCommonKlass);
void esp_device_id_clear();
/** vm_read only — call from menu Device tab or after Resolve. */
void esp_device_id_refresh(pid_t pid);
const EspDeviceIdSnap &esp_device_id_snapshot();
void esp_device_id_draw_panel(pid_t pid);