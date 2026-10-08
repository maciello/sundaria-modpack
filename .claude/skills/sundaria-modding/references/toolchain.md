# Toolchain (Linux host → Win64 DLL)

```yaml
compiler: clang-cl + lld-link (system LLVM) with mods/dos-tool/msvc-clang-toolchain.cmake
msvc_sdk: `xwin splat` output (~640 MB) → env XWIN (default <repo>/../tools/msvc)
game_sdk: Dumper-7 CppSDK → env SDK_DIR (default <repo>/../sdk/CppSDK)   # game-derived: never commit, never publish
build: just build → mods/dos-tool/build/{DoS-Tool.asi (loader), DoS-Tool.dll (mod)}
sdk_translation_units: only core/game.cpp includes SDK headers (+ Basic.cpp, CoreUObject_functions.cpp, Engine_functions.cpp)
  why: each SDK-including TU costs seconds-to-minutes of compile and MBs of headers; features stay SDK-free
host_tests: c++ -std=c++20 on SDK-free headers; `just test` compiles every mods/*/{core,features/*}/test/*_test.cpp
```

## Hot reload
```yaml
DoS-Tool.asi: tiny loader, never changes while the game runs (Ultimate ASI Loader loads *.asi)
dev_flag: Win64/dos-tool.dev present ⇒ loader polls DoS-Tool.dll mtime every 0.5 s
swap: ModStop() (kiero::shutdown restores Present/ResizeBuffers, WndProc restored, ImGui destroyed, every feature Off())
      → FreeLibrary → copy to DoS-Tool.loaded{0,1}.dll → LoadLibrary → ModStart()
install: `install -m 644` (new inode) — never cp over a loaded file
no_dev_flag: load once (what friends get)
verify: scripts/dev-check.sh — newest loaded<N>.dll size == DoS-Tool.dll size, log shows "ImGui (D3D11) initialised"
```

## Re-dump after a game patch (work-item justfile, Boss's machine)
```yaml
1: just dump-install           # winmm.dll + Dumper-7.asi into Win64, ini at <prefix>/drive_c/Dumper-7/Dumper-7.ini (DumpKey=End)
2: start game, enter a DUNGEON, press End, wait 10-60 s
3: just dump-collect           # newest CppSDK → sdk/
4: just dump-uninstall; just -f pack/justfile build test; release
blueprint_coverage: native classes always dumped; Blueprint classes only if loaded at dump time (re-dump in the village for hub-only BPs)
```

## Updater (friends)
```yaml
files: updater/update.py (Linux/Proton), updater/update.ps1 (Windows)
launch_option_linux: WINEDLLOVERRIDES="winmm=n,b" python3 /path/to/update.py %command%
launch_option_windows: powershell -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\path\to\update.ps1 %command%
flow: GitHub Releases latest → zip rooted at game dir → .modpack-version / .modpack-files (removes files dropped from the pack)
failure: any error ⇒ "skipped", game still launches
safety: rejects zip entries escaping the game root
test: just test (PWSH=/path/to/pwsh also runs the ps1 against the same fake GitHub)
override: MODPACK_API=<url> for tests
```
