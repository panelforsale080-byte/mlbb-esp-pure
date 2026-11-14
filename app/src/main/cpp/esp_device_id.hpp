#pragma once
#include <sys/types.h>

struct EspDeviceIdSnap {
    bool refreshed = false;
    char gameServerJsonId[1024] = {};
    char gameServerReportId[256] = {};
    char gameServerUniqueId[384] = {};
    char gameServerAndroidId[96] = {};
    char gameServerAdId[192] = {};
    char sdkDeviceId[384] = {};
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

/** Empty field = skip apply. Saved to overlay app files/device_spoof.cfg */
struct EspDeviceSpoofCfg {
    bool enabled = false;
    bool autoAndroidTxt = true;
    bool loaded = false;
    char gscJson[1024] = {};
    char gscReport[256] = {};
    char gscUnique[384] = {};
    char gscAndroid[96] = {};
    char gscAd[192] = {};
    char sdkDeviceId[384] = {};
    char sdkUnique[192] = {};
    char sdkAndroid[192] = {};
    char sdkAd[192] = {};
    char sdkOaid[192] = {};
    char sdkChannel[96] = {};
    char utilAndroid[192] = {};
    char utilAd[192] = {};
    char utilTelephony[192] = {};
    char utilMac[96] = {};
};

EspDeviceSpoofCfg &esp_device_id_spoof_cfg();
bool esp_device_id_spoof_save();
/** requireEnabled: only when auto in-place on Refresh; allowPtrace: Apply now (lobby only) */
bool esp_device_id_apply_spoof(pid_t pid, bool requireEnabled = true, bool allowPtrace = false);
void esp_device_id_spoof_fill_from_snapshot();
/** /storage/emulated/0/android.txt — Copy-all lines or key=value. */
bool esp_device_id_load_android_txt(bool force);