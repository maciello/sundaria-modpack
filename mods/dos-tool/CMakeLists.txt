cmake_minimum_required(VERSION 3.20)
project(DoS-Tool LANGUAGES CXX C)

# PDB per binary for crash dumps (minidump.py): functions + lines only, since full /Z7 changes codegen
# of a few functions; /OPT:REF,ICF = what a non-/DEBUG link does; the DLL records only the PDB's file name.
add_compile_options(/Z7 -gline-tables-only)
add_link_options(/DEBUG /OPT:REF /OPT:ICF /PDBALTPATH:%_PDB%)

# Generated Dumper-7 SDK for Dungeons of Sundaria (override with -DSDK_DIR=...).
# build.sh auto-discovers the CppSDK folder under the build kit's output.
set(SDK_DIR ""
    CACHE PATH "Path to the generated CppSDK folder")

set(TP "${CMAKE_SOURCE_DIR}/third_party")
set(IMGUI "${TP}/imgui")

file(GLOB FEATURES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/features/*/*.cpp" "${CMAKE_SOURCE_DIR}/features/*/*/*.cpp")
list(FILTER FEATURES EXCLUDE REGEX "/test/")  # native tests have main()

add_library(${PROJECT_NAME} SHARED
    # our code: core host + every features/[<domain>/]<name>/*.cpp (each feature registers itself) + domain shared/*.cpp
    core/dllmain.cpp
    core/game.cpp
    core/overlay.cpp
    core/ref.cpp
    core/reflect.cpp
    core/umg.cpp
    ${FEATURES}

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

    # Lua 5.4 for ability scripts (features/ability-mods), as C++
    "${TP}/lua/lua_all.cpp"
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    PREFIX ""          # no "lib" prefix
    OUTPUT_NAME "DoS-Tool"
    SUFFIX ".dll"      # loaded by DoS-Tool.asi (core/loader.cpp)
)

# Ultimate ASI Loader loads *.asi; this one loads (and in dev mode hot-reloads) DoS-Tool.dll.
add_library(DoS-Loader SHARED core/loader.cpp)
set_target_properties(DoS-Loader PROPERTIES PREFIX "" OUTPUT_NAME "DoS-Tool" SUFFIX ".asi"
    ARCHIVE_OUTPUT_NAME "DoS-Loader" PDB_NAME "DoS-Loader")  # own import lib; Ninja rejects two targets writing DoS-Tool.lib

target_include_directories(${PROJECT_NAME} PRIVATE
    core
    "${SDK_DIR}"
    "${SDK_DIR}/SDK"
    "${IMGUI}"
    "${IMGUI}/backends"
    "${TP}/kiero"
    "${TP}/lua/src"
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
    d3d11 dxgi d3dcompiler dwmapi user32 gdi32 ws2_32
)
