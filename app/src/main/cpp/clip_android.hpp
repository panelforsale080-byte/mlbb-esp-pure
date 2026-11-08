#pragma once
#include <jni.h>
#include <cstdint>
#include <vector>

void android_clip_set_context(JavaVM *vm, jobject context);
bool android_copy_to_clipboard(const char *text);
bool android_read_asset(const char *name, std::vector<uint8_t> &out);
