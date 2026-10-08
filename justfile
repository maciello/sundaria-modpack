# Dungeons of Sundaria modpack. Local build needs the game SDK + MSVC kit (not in this repo):
#   SDK_DIR = Dumper-7 CppSDK of your game build, XWIN = `xwin splat` output
sdk  := env_var_or_default("SDK_DIR", justfile_directory() / "../sdk/CppSDK")
xwin := env_var_or_default("XWIN", justfile_directory() / "../tools/msvc")
repo := "maciello/sundaria-modpack"
win64 := env_var_or_default("GAME_WIN64", env_var("HOME") / ".steam/steam/steamapps/common/DungeonsofSundaria/Archon/Binaries/Win64")

test:
    python3 updater/test_update.py   # PWSH=/path/to/pwsh also tests update.ps1
    mkdir -p build && c++ -std=c++20 -Imods/dos-tool/src mods/dos-tool/test/dmgnum_test.cpp -o build/dmgnum_test
    build/dmgnum_test

build:
    SDK_DIR={{sdk}} XWIN={{xwin}} mods/dos-tool/build.sh

# release layout = paths relative to the game root
dist: build
    rm -rf dist && mkdir -p dist/Archon/Binaries/Win64
    cp mods/dos-tool/vendor/winmm.dll mods/dos-tool/build/DoS-Tool.asi mods/dos-tool/build/DoS-Tool.dll dist/Archon/Binaries/Win64/
    rm -f build/modpack.zip && cd dist && python3 -m zipfile -c ../build/modpack.zip Archon

release tag: test dist
    gh release create {{tag}} build/modpack.zip -R {{repo}} --title {{tag}} --generate-notes

# once, game closed: hot-reload loader + dev flag
dev-install: build
    install -m 644 mods/dos-tool/build/DoS-Tool.asi mods/dos-tool/vendor/winmm.dll mods/dos-tool/build/DoS-Tool.dll "{{win64}}/"
    touch "{{win64}}/dos-tool.dev"

# game running: rebuild; the loader swaps DoS-Tool.dll in within ~1 s
dev: build
    install -m 644 mods/dos-tool/build/DoS-Tool.dll "{{win64}}/"
