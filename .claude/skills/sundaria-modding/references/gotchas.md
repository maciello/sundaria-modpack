# Gotchas (each cost a debugging round)

| symptom | cause | fix |
|---|---|---|
| Dumper-7 loads but writes nothing | under Proton cwd ≠ exe dir → local Dumper-7.ini never read → dumps at startup before UWorld exists | global ini `<prefix>/drive_c/Dumper-7/Dumper-7.ini` (`just dump-install` writes it) |
| Dumper-7 cmake: missing includes | Windows-only repo, wrong filename case | symlinks with the expected case (`upstream/Dumper-7`) |
| clang: `-Winvalid-constexpr` errors in SDK/Dumper | MSVC-only leniency | `-Wno-invalid-constexpr` in the toolchain file — NOT on the cmake command line (CMAKE_CXX_FLAGS there wipes the toolchain's INIT /imsvc flags) |
| `pgrep -f Archon…` says running when it isn't | matches your own shell AND the Steam/Proton wrappers (exe path in their args, they outlive a crash) | `pgrep -x Archon-Win64-Sh` (process name, 15-char truncated) |
| `just dist` exit 127 | `zip` not installed | `python3 -m zipfile -c` |
| updater can't find game root | no root exe; Steam runs `Archon/Binaries/Win64/Archon-Win64-Shipping.exe` | root = exe path parents[3] |
| update.ps1 fails on Linux pwsh | `$env:TEMP` unset; Get-Item on missing file | `[IO.Path]::GetTempPath()`; `Split-Path` |
| heal numbers when enemies spawn | HP fills 0 → max after spawn | ignore changes within 1.5 s of first sight and when previous HP ≤ 0 |
| mouse cursor over the game | ImGui MouseDrawCursor left on | `MouseDrawCursor = menu open` |
| reads fault in menus/lobby | camera manager isn't BP_PlayerCamera_C there | `IsA` before casting; `PtrOk` every pointer |
| no log anywhere | upstream logger hardcoded a dev's home path | log next to the module (`GetModuleHandleEx FROM_ADDRESS`) |
| commit exposes personal email in the public repo | default git author | author = GitHub noreply of the hosting account |
| GitHub/Steam downloads stall at ~2-5 GiB | Proton VPN free tier throttle | check the VPN before debugging the network |
| bundled winmm.dll hash ≠ official | old/unknown ASI loader build | official Ultimate ASI Loader release, `dinput8.dll` renamed to `winmm.dll` |
| game crash `EXCEPTION_ACCESS_VIOLATION reading 0x0` in `MSVCP140.dll` (minidump) | `std::mutex` from a newer MSVC STL (constexpr ctor) vs Proton's older msvcp140 `_Mtx_lock` | no `std::mutex`/`std::thread`/`condition_variable`: use `SRWLOCK`; check with `llvm-objdump -p DoS-Tool.dll \| grep _Mtx` → 0 |
| crash diagnosis | UE writes `<prefix>/drive_c/users/steamuser/AppData/Local/Archon/Saved/Crashes/UE4CC-*/UE4Minidump.dmp` | `scripts/minidump.py <dmp>` → fault module+offset, stack return addresses into DoS-Tool/Archon |
| command handed to the user fails in their terminal | user shell is fish: no heredocs (`<<EOF`), no `$(...)` bash-isms | put it in a `just` recipe (or a file + recipe) and hand over `just <recipe>` |
