#pragma once
#include <jni.h>
#include <cstdint>
#include <vector>

void android_clip_set_context(JavaVM *vm, jobject context);
bool android_copy_to_clipboard(const char *text);
/** Primary clip UTF-8 into out (truncated). Empty clip returns false. */
bool android_read_clipboard(char *out, size_t outSz);
/** Absolute path (e.g. /storage/emulated/0/android.txt). */
bool android_read_file_path(const char *absPath, std::vector<uint8_t> &out);
bool android_write_file_path(const char *absPath, const char *text);
bool android_read_asset(const char *name, std::vector<uint8_t> &out);
/** Read/write small UTF-8 file under Context.getFilesDir() (e.g. "device_spoof.cfg"). */
bool android_app_file_read(const char *relName, std::vector<uint8_t> &out);
bool android_app_file_write(const char *relName, const char *text);
