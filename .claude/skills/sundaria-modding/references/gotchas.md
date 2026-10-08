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
| damage numbers sum different abilities into one stack | stack key was target + 1 s window; HP diffs carry no source | key on `LastTakeHitInfo` damage type × instigator (`core/combat.hpp` ledger) |
| per-frame "HP drop ↔ hit record" pairing mis-tags | render thread samples mid game tick: record before/after its drop, drop split over 2 frames, game sums same-frame hits into one record | records CLAIM up to their damage from unshown loss within 0.1 s; unclaimed loss shows untagged; never pair by frame |
| DoT stacks split every tick | ticks ~1.0 s apart == old 1.0 s stack window | stack window 1.3 s (< 1.4 s lifetime) |
| crash diagnosis | UE writes `<prefix>/drive_c/users/steamuser/AppData/Local/Archon/Saved/Crashes/UE4CC-*/UE4Minidump.dmp` | `scripts/minidump.py <dmp>` → fault module+offset, stack return addresses into DoS-Tool/Archon |
| grep finds no `UnrealContainers.hpp` | it sits at `CppSDK/UnrealContainers.hpp`, not under `CppSDK/SDK/` (also `UtfN.hpp`, `NameCollisions.inl`) | search `CppSDK/` recursively, or `scripts/sdk.py` |
| SDK `TMap` loop / `operator[]` won't compile | `SetElement::Value` is private in the Dumper-7 containers | read the sparse array raw (`game-facts.md` sdk_quirk) |
| per-cast state carries into the next cast (input-feel `g_outAbility`, unverified) | GAS abilities can be instanced per actor: the same `UGameplayAbility*` every cast | reset per-cast state when `AnimatingAbility` changes or goes null, never key on the pointer alone |
| subagent's SDK header reads denied by the permission check | Claude Code auto-mode classifier, not a repo rule | ask the user; the main session can write the needed excerpt to `../research/*.txt` (outside the repo) for the agent |
| command handed to the user fails in their terminal | user shell is fish: no heredocs (`<<EOF`), no `$(...)` bash-isms | put it in a `just` recipe (or a file + recipe) and hand over `just <recipe>` |
- Feature toggles persist as `<name>=<on>,<stage>` in `dos-tool.ini`. A choice saved under another stage (or the old `<name>=<on>` form) is ignored, so a promotion (Beta → Stable) actually turns the feature on for players who never touched it. Re-tick after a stage change if you want a non-default.
- `dos-tool.ini` stores only toggles that differ from the default (`Default()` in `core/overlay.cpp`). Writing every toggle froze whatever default was current at first save.
- Native (non-UPROPERTY) field offsets: scan the object's floats twice a few seconds apart and log those that moved by the elapsed time; match against the UE source layout and the Dumper-7 `Pad_*` gap. Guess-by-order was wrong once (0x1FC read 0).
- Skill scripts run through the `~/.claude/skills` symlink: resolve their own path (`readlink -f "$0"`, Python `Path(__file__).resolve()`), else repo-relative paths land in `$HOME` (new-feature.sh once scaffolded into `~/mods/`).
