cmake_minimum_required(VERSION 3.20)
project(DoS-Tool LANGUAGES CXX C)

# Generated Dumper-7 SDK for Dungeons of Sundaria (override with -DSDK_DIR=...).
# build.sh auto-discovers the CppSDK folder under the build kit's output.
set(SDK_DIR ""
    CACHE PATH "Path to the generated CppSDK folder")

set(TP "${CMAKE_SOURCE_DIR}/third_party")
set(IMGUI "${TP}/imgui")

add_library(${PROJECT_NAME} SHARED
    # our code
    src/dllmain.cpp
    src/game.cpp
    src/overlay.cpp

    # generated SDK: only the units whose function bodies we actually reference
    "${SDK_DIR}/SDK/Basic.cpp"
    "${SDK_DIR}/SDK/CoreUObject_functions.cpp"
    "${SDK_DIR}/SDK/Engine_functions.cpp"

    # Dear ImGui core + D3D11/Win32 backends
    "${IMGUI}/imgui.cpp"
    "${IMGUI}/imgui_draw.cpp"
    "${IMGUI}/imgui_tables.cpp"
    "${IMGUI}/imgui_widgets.cpp"
    "${IMGUI}/backends/imgui_impl_dx11.cpp"
    "${IMGUI}/backends/imgui_impl_win32.cpp"

    # kiero (D3D11 vtable hooking) + its bundled MinHook
    "${TP}/kiero/kiero.cpp"
    "${TP}/kiero/minhook/src/hook.c"
    "${TP}/kiero/minhook/src/buffer.c"
    "${TP}/kiero/minhook/src/trampoline.c"
    "${TP}/kiero/minhook/src/hde/hde64.c"
    "${TP}/kiero/minhook/src/hde/hde32.c"
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    PREFIX ""          # no "lib" prefix
    OUTPUT_NAME "DoS-Tool"
    SUFFIX ".dll"      # loaded by DoS-Tool.asi (src/loader.cpp)
)

# Ultimate ASI Loader loads *.asi; this one loads (and in dev mode hot-reloads) DoS-Tool.dll.
add_library(DoS-Loader SHARED src/loader.cpp)
set_target_properties(DoS-Loader PROPERTIES PREFIX "" OUTPUT_NAME "DoS-Tool" SUFFIX ".asi")

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    "${SDK_DIR}"
    "${SDK_DIR}/SDK"
    "${IMGUI}"
    "${IMGUI}/backends"
    "${TP}/kiero"
)

# MSVC-emulation flags so the MSVC-targeted SDK + Windows headers compile under
# clang, and silence the (very noisy but harmless) generated-SDK warnings.
target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>
    $<$<COMPILE_LANGUAGE:CXX>:-fms-compatibility>
    $<$<COMPILE_LANGUAGE:CXX>:-fms-compatibility-version=19.29>
    $<$<COMPILE_LANGUAGE:CXX>:-Xclang>
    $<$<COMPILE_LANGUAGE:CXX>:-fdelayed-template-parsing>
    $<$<COMPILE_LANGUAGE:CXX>:-mavx2>
    $<$<COMPILE_LANGUAGE:CXX>:-msse4.2>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-c++11-narrowing>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-narrowing>
    -Wno-error
    -w
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)

# imgui win32 backend links dwmapi; dx11 backend needs d3d11 + d3dcompiler.
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11 dxgi d3dcompiler dwmapi user32 gdi32
)
