---
name: sundaria-modding
description: Build, extend, debug and ship mods for Dungeons of Sundaria (UE 4.27, Steam 587520) in the sundaria-modpack repo — the dos-tool DLL (ImGui overlay, damage numbers, DPS meter, camera, air control, health bars), the Dumper-7 SDK, hot reload into the running game, and the GitHub-release auto-updater friends use. Use whenever the user mentions Sundaria, the modpack, dos-tool, a new in-game feature/tweak (UI, numbers, movement, camera, stats), "the game patched and the mod broke", re-dumping the SDK, releasing to friends, or finding a game class/field/offset — even if they don't say "mod".
---

# Sundaria modding

```yaml
repo: maciello/sundaria-modpack (public)   # local: <work-item>/pack, conventions in pack/CLAUDE.md
not_in_repo: [../sdk/CppSDK (game-derived, never publish), ../tools/msvc (xwin), ../upstream/Dumper-7]
mod: mods/dos-tool = DoS-Tool.asi (hot-reload loader) + DoS-Tool.dll (core host + features)
runtime: Windows build under Proton; launch option WINEDLLOVERRIDES="winmm=n,b" %command%
```

## Layout → where a change goes
```yaml
core/:   # shared; features never include each other
  game.hpp/.cpp: the ONLY SDK translation unit; add a small SDK-free function here for any new game read/write
  combat.hpp: health-diff → stacked hit events, typical hit, fight/DPS (consumed via Frame.combat)
  feature.hpp: Feature {name, enabled, OnFrame(Frame), Menu(), Off()}; Frame {now, w, h, font, snap, combat}
  overlay.cpp: D3D11 Present hook, ImGui, Insert menu, runs every feature
  draw.hpp: FormatAmount, OutlinedText
features/<kebab>/:   # one player-facing feature per folder; CMake globs features/*/*.cpp
  <kebab>.cpp: one static Feature object; registers itself
  <kebab>.hpp + test/<kebab>_test.cpp: SDK-free logic + assert test (picked up by `just test`)
```

## Workflows

**New feature** — `scripts/new-feature.sh <kebab> "<Menu Title>" [--logic]` scaffolds from `assets/`.
Then: find the game data (`scripts/sdk.py`), add the read/write to `core/game.*`, logic + test in the
feature's header, drawing/applying in `OnFrame`, restore vanilla in `Off`.

**Find game data** — never grep the 140 MB SDK by hand:
```bash
scripts/sdk.py field 'Health|AirControl'   # owner::member @offset (type) file:line
scripts/sdk.py class UCharacterMovementComponent
scripts/sdk.py chain AArchonCharacter      # → ACharacter → APawn → AActor → UObject
scripts/sdk.py subs AArchonCharacter       # what IsA() will catch
```
Known paths (health, camera, movement, combat log) are in `references/game-facts.md` — read it before the SDK.

**Dev loop** (from pack/):
```bash
just test          # every host test + updater
just dev           # build + hot-swap into the running game (~1 s); first time: just dev-install with game closed
scripts/dev-check.sh   # game running? swap happened? log tail
```
A swap is verified only when dev-check shows the newest `loaded<N>.dll` size equals `DoS-Tool.dll` and the log re-printed `ImGui (D3D11) initialised`. What it looks like in game is the user's call — say "unverified visually" until they confirm.

**Ship to friends** — `just release vX.Y.Z` (test → dist zip rooted at game dir → GitHub release). Friends' updater installs it on next launch. Ask Boss before releasing; it reaches other people's machines.

**Game patched** — re-dump → rebuild → release; steps in `references/toolchain.md`.

## Rules and why
- Render thread (`OnFrame`) does memory reads/writes only, no `ProcessEvent`/UFunction calls: UE isn't thread-safe there and it crashes intermittently.
- Check every pointer (`PtrOk`) and `IsA` before casting: the same objects are different classes in menus/lobby, and actors die between frames.
- Movement/stats are server-simulated: apply to every player character, and tell the user co-op only works when the host runs the pack.
- `Off()` must restore the originals you captured. Only restore objects found in the live world; stale pointers may be freed.
- Logic with branches/math goes into an SDK-free header with a test, because the in-game loop can't be unit-tested.
- Public repo: no SDK, no personal email (commit as the account's GitHub noreply), no absolute home paths.

## References
| file | read when |
|---|---|
| `references/game-facts.md` | touching any game object: paths, offsets, what is verified, combat-log option |
| `references/toolchain.md` | build/compile errors, hot reload internals, re-dump, updater |
| `references/gotchas.md` | something fails: known symptom → cause → fix table |
