#!/usr/bin/env bash
# Build DoS-Tool.asi (Windows x64) on Linux using the same clang -> MSVC-ABI
# toolchain + xwin SDK that built Dumper-7.dll and the PAYDAY3 tool.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
XWIN="${XWIN:-$HERE/../tools/msvc}"
BUILD="$HERE/build"

[[ -d "$XWIN/crt" ]] || { echo "!! Missing $XWIN (xwin splat output)"; exit 1; }

# Auto-discover the generated CppSDK folder (Dumper-7 nests it under a version dir).
SDK_DIR="${SDK_DIR:-$HERE/../sdk/CppSDK}"
[[ -n "$SDK_DIR" && -d "$SDK_DIR/SDK" ]] || {
    echo "!! No DoS CppSDK at $SDK_DIR."
    echo "   Generate it first (inject Dumper-7 into DoS, press End), then copy it there."
    exit 1
}
echo ">> Using SDK: $SDK_DIR"

cmake -S "$HERE" -B "$BUILD" -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$HERE/msvc-clang-toolchain.cmake" \
  -DXWIN_SDK="$XWIN" \
  -DSDK_DIR="$SDK_DIR" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build "$BUILD" --config Release -j"$(nproc)"

echo ">> Built: $(find "$BUILD" -name 'DoS-Tool.*')"
