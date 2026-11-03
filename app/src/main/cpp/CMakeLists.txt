cmake_minimum_required(VERSION 3.22.1)
project(MlbbEsp)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(MlbbEsp SHARED
    app_main.cpp
    proc.cpp
    mem.cpp
    esp_engine.cpp
    ui_log.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_impl_android.cpp
    ${IMGUI_DIR}/imgui_impl_opengl3.cpp)

target_include_directories(MlbbEsp PRIVATE
    ${IMGUI_DIR}
    ${ANDROID_NDK}/sources/android/native_app_glue)

target_compile_definitions(MlbbEsp PRIVATE
    IMGUI_IMPL_OPENGL_ES3
    GLES_SILENCE_DEPRECATION)

find_library(log-lib log)
find_library(android-lib android)
find_library(EGL-lib EGL)
find_library(GLESv3-lib GLESv3)

target_link_libraries(MlbbEsp
    native_app_glue
    ${log-lib}
    ${android-lib}
    ${EGL-lib}
    ${GLESv3-lib})

target_link_options(MlbbEsp PRIVATE
    "-Wl,-u,ANativeActivity_onCreate")
