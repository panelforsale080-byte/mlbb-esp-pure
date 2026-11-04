#pragma once
#include <jni.h>

struct android_app;
bool android_copy_to_clipboard(android_app *app, const char *text);
void android_clip_set_context(JavaVM *vm, jobject context);
bool android_copy_to_clipboard(const char *text);
