#pragma once
struct android_app;
bool android_copy_to_clipboard(android_app *app, const char *text);
