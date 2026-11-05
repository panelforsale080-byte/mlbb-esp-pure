cmake_minimum_required(VERSION 3.22.1)
project(MlbbEsp)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

add_library(MlbbEsp SHARED
    ui_core.cpp
    overlay_jni.cpp
    proc.cpp
    mem.cpp
    esp_engine.cpp
    esp_draw.cpp
    esp_camera.cpp
    conrent_esp.cpp
    remote_call.cpp
    ui_log.cpp
    remote_il2cpp.cpp
    remote_elf.cpp
    clip_android.cpp
    esp_w2s_bridge.cpp
    esp_inject.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_impl_android.cpp
    ${IMGUI_DIR}/imgui_impl_opengl3.cpp)

target_include_directories(MlbbEsp PRIVATE ${IMGUI_DIR})

target_compile_definitions(MlbbEsp PRIVATE
    IMGUI_IMPL_OPENGL_ES2
    GLES_SILENCE_DEPRECATION)

find_library(log-lib log)
find_library(android-lib android)
find_library(EGL-lib EGL)
find_library(GLESv2-lib GLESv2)

target_link_libraries(MlbbEsp
    ${log-lib}
    ${android-lib}
    ${EGL-lib}
    ${GLESv2-lib})

add_library(mlbb_espw2s SHARED inject_agent/esp_w2s_agent.cpp)
target_link_libraries(mlbb_espw2s ${log-lib})
