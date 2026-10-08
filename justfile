# Dungeons of Sundaria modpack. Local build needs the game SDK + MSVC kit (not in this repo):
#   SDK_DIR = Dumper-7 CppSDK of your game build, XWIN = `xwin splat` output
buildid := `grep -Pos '"buildid"\s+"\K[0-9]+' "$HOME/.steam/steam/steamapps/appmanifest_587520.acf" || true`
store_sdk := "/srv/dumps/sundaria/" + buildid + "/CppSDK"
sdk  := env_var_or_default("SDK_DIR", if path_exists(store_sdk) == "true" { store_sdk } else { justfile_directory() / "../sdk/CppSDK" })
xwin := env_var_or_default("XWIN", if path_exists("/srv/toolchains/xwin-msvc") == "true" { "/srv/toolchains/xwin-msvc" } else { justfile_directory() / "../tools/msvc" })
repo := "maciello/sundaria-modpack"
win64 := env_var_or_default("GAME_WIN64", env_var("HOME") / ".steam/steam/steamapps/common/DungeonsofSundaria/Archon/Binaries/Win64")

# SDK for the installed game build from the shared dump store (ssh alias `dumps`)
sdk-pull host="dumps":
    mkdir -p {{sdk}} && rsync -a --delete {{host}}:/srv/dumps/sundaria/{{buildid}}/CppSDK/ {{sdk}}/

# start of work: get the other's commits
sync:
    git pull --rebase
    git log --oneline -10

# end of work: test, rebase on the other's commits, push
ship: test
    git pull --rebase
    git push

test:
    python3 updater/test_update.py   # PWSH=/path/to/pwsh also tests update.ps1
    mkdir -p build && for t in mods/*/core/test/*_test.cpp mods/*/features/*/test/*_test.cpp; do m=$(dirname $(dirname $t)); c++ -std=c++20 -I$m -I$(echo $t | cut -d/ -f1-2)/core $t -o build/$(basename $t .cpp) && build/$(basename $t .cpp) || exit 1; done

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
