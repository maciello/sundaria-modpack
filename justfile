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
    just data-setup
    just tables >/dev/null && echo "game tables db: build/game-data.sqlite (just tables <text>)" || echo "game tables db skipped (game not found); later: just tables"

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
    for d in $(find mods/dos-tool/features -name '*.cpp' -not -path '*/test/*' -not -path '*/shared/*' -exec dirname {} \; | sort -u); do mk "mod:$(basename $d)" 5319E7 "${d#mods/dos-tool/}"; done

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
    sh scripts/version.sh check
    sh scripts/gobjects-check.sh
    sh scripts/actor-walk-check.sh
    sh scripts/widget-off-check.sh
    {{python}} scripts/ref-check.py --self-test && {{python}} scripts/ref-check.py
    {{python}} scripts/data.py --self-test
    {{python}} scripts/game.py --self-test
    {{python}} scripts/abilities.py --self-test
    {{python}} updater/test_update.py   # PWSH=/path/to/pwsh also tests update.ps1
    mkdir -p build && for t in mods/*/core/test/*_test.cpp mods/*/features/*/test/*_test.cpp mods/*/features/*/*/test/*_test.cpp; do m=$(dirname $(dirname $t)); {{cxx}} -std=c++20 -I$m -I$(echo $t | cut -d/ -f1-2)/core $t -o build/$(basename $t .cpp){{exe}} && build/$(basename $t .cpp){{exe}} || exit 1; done
    for t in libs/*/test/*_test.cpp; do l=$(dirname $(dirname $t)); b=build/$(basename $l)_$(basename $t .cpp){{exe}}; {{cxx}} -std=c++20 -O2 -I$l/include $t $l/src/*.cpp -o $b && $b || exit 1; done   # SDK-free libraries (libs/<lib>/{include,src,test})

# DPS library (#118): tables file from the extracted pak data (game data, outside the repo); next to the exe the DLL reads it too
dps_tables := env_var_or_default("DPS_TABLES", justfile_directory() / "build/dps/tables.txt")  # never the game dir: `just dps-install` is the only writer there

# no game needed: hero <snapshot.yaml> | score [--items F..] | bis <Class> [--level L] | weights <snapshot|Class> | calibrate <snapshot> | bench
[positional-arguments]
dps *args: dps-lib
    @DPS_TABLES="{{dps_tables}}" DPS_CHARS="{{win64}}/dos-tool-chars" {{python}} scripts/dps.py "$@"

# libs/dps -> build/libdps.so for scripts/dps.py (ctypes)
dps-lib:
    #!/usr/bin/env bash
    set -euo pipefail
    so=build/libdps.so
    [ -f $so ] && [ -z "$(find libs/dps -type f -newer $so)" ] && exit 0
    mkdir -p build && c++ -std=c++20 -O3 -march=native -shared -fPIC -pthread -Ilibs/dps/include libs/dps/src/*.cpp libs/dps/host/*.cpp -o $so

# <model.json> (pak extract, dps-sim `just extract`) -> the tables file `just dps` reads
dps-tables model out=dps_tables:
    mkdir -p "$(dirname "{{out}}")"
    {{python}} scripts/dps_tables.py "{{model}}" "{{out}}"

# the tables file -> <Win64>/dos-tool-dps/tables.txt, where item upgrade reads it in game (until the in-game Source, #118)
dps-install:
    mkdir -p "{{win64}}/dos-tool-dps" && cp "{{dps_tables}}" "{{win64}}/dos-tool-dps/tables.txt"

# offline game data from the pak: find <regex> [--class C] | show <asset> | table <asset> | bp <asset> [fn] | grep <regex> [--in <path regex>]
[positional-arguments]
data *args:
    @PAKS="{{win64}}/../../Content/Paks" {{python}} scripts/data.py "$@"

# game crash dumps (UE writes <Proton prefix>/drive_c/users/steamuser/AppData/Local/Archon/Saved/Crashes/UE4CC-*/UE4Minidump.dmp): newest first, error + fault site; details: `.claude/skills/sundaria-modding/scripts/minidump.py [dmp]`
crashes n="10":
    #!/usr/bin/env bash
    d="{{win64}}/../../../../../compatdata/587520/pfx/drive_c/users/steamuser/AppData/Local/Archon/Saved/Crashes"
    [ -d "$d" ] || d="$(ls -d ~/.steam/steam/steamapps/compatdata/587520/pfx/drive_c/users/steamuser/AppData/Local/Archon/Saved/Crashes 2>/dev/null)"
    [ -d "$d" ] || { echo "no Crashes folder (Windows: %LOCALAPPDATA%\\Archon\\Saved\\Crashes)"; exit 1; }
    ls -t "$d" | head -{{n}} | while read c; do
      f=$(python3 .claude/skills/sundaria-modding/scripts/minidump.py "$d/$c/UE4Minidump.dmp" 2>&1 | grep -m1 '^fault' | cut -d' ' -f2)
      e=$(grep -o '<ErrorMessage>[^<]*' "$d/$c/CrashContext.runtime-xml" | cut -c15- | head -c 90)
      echo "$(date -r "$d/$c" '+%m-%d %H:%M')  ${f:-?}  $e  $c"
    done

# all DataTables/CurveTables in build/game-data.sqlite (rebuilt when the pak changes): no arg = counts, <text> = full-text search (table, row, snippet)
[positional-arguments]
tables *args:
    @PAKS="{{win64}}/../../Content/Paks" {{python}} scripts/tables.py "$@"

# raw SQL on the tables db: tables(name,path,kind,row_struct,rows,columns) rows(tbl,row,json) curves(tbl,row,x,y) fts(tbl,row,text)
[positional-arguments]
tables-sql sql:
    @PAKS="{{win64}}/../../Content/Paks" {{python}} scripts/tables.py --sql "$1"

# every item id of the game (common: currency, essence, crafting, consumables …; armor; weapon) from the pak, cached in build/items/; `just items <regex>` filters by id or name
items regex=".":
    #!/usr/bin/env bash
    set -e
    c=build/items; mkdir -p $c
    for t in CommonItem/ItemTable_CommonItem ArmorItem/ItemTable_Armor WeaponItem/ItemTable_Weapon; do
      f=$c/$(basename $t).yaml; [ -s $f ] || just data table Archon/Content/DataTables/Item/$t > $f
    done
    {{python}} - "{{regex}}" $c/*.yaml <<'PY'
    import re, sys, yaml
    rx = re.compile(sys.argv[1], re.I)
    for f in sys.argv[2:]:
        for table, rows in (yaml.safe_load(open(f)) or {}).items():
            for i, r in rows.items():
                name = r.get("mDisplayName") or r.get("DisplayName") or ""
                line = f"{i}\t{table.replace('ItemTable_', '')}\tq{r.get('Quality', '')}\tstack {r.get('MaxStack', '')}\t{name}"
                if rx.search(line): print(line)
    PY

# abilities in the DPS tables as YAML by class (cooldown, coef, damage source, weapon types, sections, primary); regex = class or name, case-insensitive
[positional-arguments]
abilities *args:
    @{{python}} scripts/abilities.py --tables "{{dps_tables}}" "$@"

# game running (dev install): ask the live game, YAML out: get <path> | find <class> | call | trace <regex> <s> | shot | log | feature [<name> on|off] | ping
[positional-arguments]
game *args:
    @GAME_WIN64="{{win64}}" {{python}} scripts/game.py "$@"

# once per machine: .NET 10 SDK for `just data`, into ../tools/dotnet next to the main clone (skipped if one is on PATH)
data-setup:
    dotnet --list-sdks 2>/dev/null | grep -q '^10\.' || { t="$(git rev-parse --path-format=absolute --git-common-dir)/../../tools"; mkdir -p "$t" && curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 10.0 --install-dir "$t/dotnet" --no-path; }

# fetches SDK + MSVC kit first if this machine has none
build:
    [ -d "{{sdk}}/SDK" ] && [ -d "{{xwin}}/crt" ] || just sdk-pull
    SDK_DIR="{{sdk}}" XWIN="{{xwin}}" mods/dos-tool/build.sh

# release layout = paths relative to the game root
dist:
    DOS_LOCAL=OFF just build
    grep -q "^DOS_LOCAL:BOOL=OFF" mods/dos-tool/build/CMakeCache.txt || { echo "dist: local/ features compiled in, refusing"; exit 1; }
    rm -rf dist && mkdir -p dist/Archon/Binaries/Win64
    cp mods/dos-tool/vendor/winmm.dll mods/dos-tool/build/DoS-Tool.asi mods/dos-tool/build/DoS-Tool.dll dist/Archon/Binaries/Win64/
    mkdir -p dist/Archon/Binaries/Win64/dos-mods/abilities && cp mods/dos-tool/abilities/_*.lua dist/Archon/Binaries/Win64/dos-mods/abilities/
    rm -f build/modpack.zip && cd dist && {{python}} -m zipfile -c ../build/modpack.zip Archon

# next SemVer tag + release notes from git (rule: .claude/rules/versioning.md); nothing is created
next-version:
    @sh scripts/version.sh next
    @sh scripts/version.sh notes

# ship to friends: version computed by scripts/version.sh; only from a clean tree that IS origin/master, tag = the built commit
release:
    #!/usr/bin/env bash
    set -euo pipefail
    git fetch -q origin master --tags
    [ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "dirty tree: commit or move changes first"; exit 1; }
    [ "$(git rev-parse HEAD)" = "$(git rev-parse origin/master)" ] || { echo "HEAD != origin/master: just ship (or checkout origin/master) first"; exit 1; }
    tag=$(sh scripts/version.sh next); mkdir -p build; sh scripts/version.sh notes > build/notes.md
    echo "releasing $tag"; cat build/notes.md
    just test dist
    gh release create "$tag" build/modpack.zip -R {{repo}} --title "$tag" --notes-file build/notes.md --target "$(git rev-parse HEAD)"

# once, game closed: hot-reload loader + dev flag (dev builds also compile gitignored mods/dos-tool/local/)
dev-install:
    DOS_LOCAL=ON just build
    install -m 644 mods/dos-tool/build/DoS-Tool.asi mods/dos-tool/vendor/winmm.dll mods/dos-tool/build/DoS-Tool.dll mods/dos-tool/build/DoS-Tool.pdb "{{win64}}/"
    touch "{{win64}}/dos-tool.dev"
    mkdir -p "{{win64}}/dos-mods/abilities" && install -m 644 mods/dos-tool/abilities/_*.lua "{{win64}}/dos-mods/abilities/"

# game running (dev install): dump live UMG trees + styles (roots whose class contains any of the words) and print the YAML path
ui *classes:
    #!/usr/bin/env bash
    set -euo pipefail
    req="{{win64}}/dos-tool-ui.request"; out="{{win64}}/dos-tool-ui.yaml"
    echo "{{classes}}" > "$req"
    for _ in $(seq 50); do [ -e "$req" ] || break; sleep 0.2; done
    [ ! -e "$req" ] || { echo "pending: open that screen in game (request kept), or UI probe not loaded (just dev)"; exit 1; }
    sleep 0.3; echo "$out"

# game running: rebuild; the loader swaps DoS-Tool.dll in within ~1 s
dev:
    DOS_LOCAL=ON just build
    install -m 644 mods/dos-tool/build/DoS-Tool.pdb mods/dos-tool/build/DoS-Tool.dll "{{win64}}/"  # PDB first: minidump.py pairs it with the DLL
    mkdir -p "{{win64}}/dos-mods/abilities" && install -m 644 mods/dos-tool/abilities/_*.lua "{{win64}}/dos-mods/abilities/"
