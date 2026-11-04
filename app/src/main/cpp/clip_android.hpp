#pragma once
#include <jni.h>

void android_clip_set_context(JavaVM *vm, jobject context);
bool android_copy_to_clipboard(const char *text);
