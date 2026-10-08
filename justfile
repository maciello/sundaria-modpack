# Dungeons of Sundaria modpack. Local build needs the game SDK + MSVC kit (not in this repo):
#   SDK_DIR = Dumper-7 CppSDK of your game build, XWIN = `xwin splat` output
# Windows: needs Git for Windows (bash) + LLVM; put GAME_WIN64=<your>/steamapps/common/DungeonsofSundaria/Archon/Binaries/Win64 in .env
set windows-shell := ["C:/Program Files/Git/bin/bash.exe", "-cu"]
set dotenv-load

steam := if os_family() == "windows" { "C:/Program Files (x86)/Steam" } else { env_var_or_default("HOME", "") / ".steam/steam" }
win64 := env_var_or_default("GAME_WIN64", steam / "steamapps/common/DungeonsofSundaria/Archon/Binaries/Win64")
# Win64 -> Binaries -> Archon -> DungeonsofSundaria -> common -> steamapps
buildid := `grep -Pohs '"buildid"\s+"\K[0-9]+' "${GAME_WIN64:-x}/../../../../../appmanifest_587520.acf" "$HOME/.steam/steam/steamapps/appmanifest_587520.acf" "/c/Program Files (x86)/Steam/steamapps/appmanifest_587520.acf" 2>/dev/null | head -1 || true`
root := replace(justfile_directory(), '\', '/')  # bash eats Windows backslashes
store_sdk := "/srv/dumps/sundaria/" + buildid + "/CppSDK"
sdk  := env_var_or_default("SDK_DIR", if path_exists(store_sdk) == "true" { store_sdk } else { root / "../sdk/CppSDK" })
xwin := env_var_or_default("XWIN", if path_exists("/srv/toolchains/xwin-msvc") == "true" { "/srv/toolchains/xwin-msvc" } else { root / "../tools/msvc" })
repo := "maciello/sundaria-modpack"
python := if os_family() == "windows" { "python" } else { "python3" }
# native tests: host c++ on Linux; on Windows clang++ against the MSVC kit (no Visual Studio needed)
cxx := if os_family() == "windows" { "clang++ --target=x86_64-pc-windows-msvc -fuse-ld=lld -isystem " + xwin + "/crt/include -isystem " + xwin + "/sdk/include/ucrt -isystem " + xwin + "/sdk/include/um -isystem " + xwin + "/sdk/include/shared -L" + xwin + "/crt/lib/x86_64 -L" + xwin + "/sdk/lib/um/x86_64 -L" + xwin + "/sdk/lib/ucrt/x86_64" } else { "c++" }
exe := if os_family() == "windows" { ".exe" } else { "" }

# SDK for the installed game build + MSVC kit from the shared dump store (ssh alias `dumps`)
sdk-pull host="dumps":
    [ -n "{{buildid}}" ] || { echo "!! no game build id: set GAME_WIN64 (see top of justfile)"; exit 1; }
    mkdir -p "{{sdk}}" "{{xwin}}"
    if command -v rsync >/dev/null; then \
      rsync -a --delete {{host}}:/srv/dumps/sundaria/{{buildid}}/CppSDK/ "{{sdk}}/" && \
      rsync -a --delete {{host}}:/srv/toolchains/xwin-msvc/ "{{xwin}}/"; \
    else \
      rm -rf "{{sdk}}"/* "{{xwin}}"/* && \
      ssh {{host}} "tar -C /srv/dumps/sundaria/{{buildid}}/CppSDK -cf - ." | tar -C "{{sdk}}" -xf - && \
      ssh {{host}} "cd /srv/toolchains/xwin-msvc && find . ! -type l ! -type d -print0 | tar --null --no-recursion -T - -cf -" | tar -C "{{xwin}}" -xf -; \
    fi  # no rsync (Windows): tar, minus the kit's case-alias symlinks a case-insensitive fs doesn't need

# once per clone: rebase on pull, run `just test` before every push
setup:
    git config pull.rebase true
    git config core.hooksPath .githooks

# start of work: get the other's commits
sync:
    git pull --rebase
    git log --oneline -10

# end of work: rebase on the other's commits, test (pre-push hook), push. Works from any branch or worktree.
ship:
    git pull --rebase origin master
    git push origin HEAD:master

# type/prio labels + one mod:<folder> label per feature folder (idempotent)
labels:
    #!/usr/bin/env bash
    set -euo pipefail
    mk() { gh label create "$1" -R {{repo}} --color "$2" --description "$3" --force >/dev/null && echo "label: $1"; }
    mk type:epic 3E4B9E "player goal spanning several mods"
    mk type:feature 0E8A16 "one mod / one capability"
    mk type:task C5DEF5 "commit-sized step of a feature"
    mk type:bug D73A4A "something behaves wrong"
    for p in 0 1 2 3; do mk prio:P$p FBCA04 "priority $p (0 = now)"; done
    mk mod:core 5319E7 "mods/dos-tool/core"
    for d in $(find mods/dos-tool/features -name '*.cpp' -not -path '*/test/*' -exec dirname {} \; | sort -u); do mk "mod:$(basename $d)" 5319E7 "${d#mods/dos-tool/}"; done

# repo admin, once: apply .github/rulesets/*.json to GitHub (skips names that already exist)
protect:
    #!/usr/bin/env bash
    set -euo pipefail
    have=$(gh api repos/{{repo}}/rulesets --jq '.[].name')
    for f in .github/rulesets/*.json; do
      name=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["name"])' "$f")
      grep -qxF "$name" <<< "$have" && { echo "exists: $name"; continue; }
      gh api -X POST repos/{{repo}}/rulesets --input "$f" --jq '"created: " + .name'
    done

test:
    {{python}} updater/test_update.py   # PWSH=/path/to/pwsh also tests update.ps1
    mkdir -p build && for t in mods/*/core/test/*_test.cpp mods/*/features/*/test/*_test.cpp; do m=$(dirname $(dirname $t)); {{cxx}} -std=c++20 -I$m -I$(echo $t | cut -d/ -f1-2)/core $t -o build/$(basename $t .cpp){{exe}} && build/$(basename $t .cpp){{exe}} || exit 1; done

# fetches SDK + MSVC kit first if this machine has none
build:
    [ -d "{{sdk}}/SDK" ] && [ -d "{{xwin}}/crt" ] || just sdk-pull
    SDK_DIR="{{sdk}}" XWIN="{{xwin}}" mods/dos-tool/build.sh

# release layout = paths relative to the game root
dist: build
    rm -rf dist && mkdir -p dist/Archon/Binaries/Win64
    cp mods/dos-tool/vendor/winmm.dll mods/dos-tool/build/DoS-Tool.asi mods/dos-tool/build/DoS-Tool.dll dist/Archon/Binaries/Win64/
    rm -f build/modpack.zip && cd dist && {{python}} -m zipfile -c ../build/modpack.zip Archon

# ship to friends: only from a clean tree that IS origin/master, tag = the built commit
release tag:
    #!/usr/bin/env bash
    set -euo pipefail
    git fetch -q origin master
    [ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "dirty tree: commit or move changes first"; exit 1; }
    [ "$(git rev-parse HEAD)" = "$(git rev-parse origin/master)" ] || { echo "HEAD != origin/master: just ship (or checkout origin/master) first"; exit 1; }
    just test dist
    gh release create {{tag}} build/modpack.zip -R {{repo}} --title {{tag}} --generate-notes --target "$(git rev-parse HEAD)"

# once, game closed: hot-reload loader + dev flag
dev-install: build
    install -m 644 mods/dos-tool/build/DoS-Tool.asi mods/dos-tool/vendor/winmm.dll mods/dos-tool/build/DoS-Tool.dll "{{win64}}/"
    touch "{{win64}}/dos-tool.dev"

# game running: rebuild; the loader swaps DoS-Tool.dll in within ~1 s
dev: build
    install -m 644 mods/dos-tool/build/DoS-Tool.dll "{{win64}}/"
