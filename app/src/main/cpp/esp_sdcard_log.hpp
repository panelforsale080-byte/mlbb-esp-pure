#pragma once
#include "conrent_esp.hpp"
#include "esp_engine.hpp"
#include "remote_il2cpp.hpp"
#include <sys/types.h>

/** conRentV1-style auto .txt on sdcard (app external files + Download fallback). */
void esp_sdcard_log_after_resolve(pid_t pid, const EspResolveReport &rep, const ConrentResolve &cr,
                                  const RemoteIl2CppInfo &il2);
void esp_sdcard_log_after_tick(pid_t pid, const EspFrame &frame, bool espDrawActive);

const char *esp_sdcard_log_dir();
const char *esp_sdcard_log_full_path();
