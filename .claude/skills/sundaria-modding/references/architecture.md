# Architecture: where a change goes, and the rules

```yaml
repo: maciello/sundaria-modpack (public)   # local: <work-item>/pack, conventions also in pack/CLAUDE.md
not_in_repo: [../sdk/CppSDK (game-derived, never publish), ../tools/msvc (xwin), ../upstream/Dumper-7]
mod: mods/dos-tool = DoS-Tool.asi (hot-reload loader) + DoS-Tool.dll (core host + features)
runtime: Windows build under Proton; launch option WINEDLLOVERRIDES="winmm=n,b" %command%

core/:   # shared; features never include each other
  game.hpp/.cpp: the ONLY core SDK translation unit; add a small SDK-free function here for any new game read/write. Never includes a feature header (core → feature is the wrong direction)
  game::SetEventListener(fn, on): game-thread ProcessEvent listener for a feature with its own SDK .cpp (≤8, a full table is logged; hook installed while any is on). The only place UFunction calls are allowed
  game::SetEventFilter(fn, on): runs before the game's call; true skips it (one filter; item-sort replaces the game's Sort click)
  ref.hpp: ref::Ref / Cached / Fn, the only place an engine pointer outlives a call (validated O(1) by GObjects index + name, re-resolved after map travel)
  style.hpp: design tokens (colours, sizes, easing) → design-system.md
  combat.hpp: health-diff → stacked hit events, typical hit, fight/DPS (Frame.combat)
  feature.hpp: Feature {name, stage (Alpha|Beta|Stable|Deprecated), enabled, OnFrame(Frame), Menu(), Off()}; on/off + font persist in dos-tool.ini (local); Frame {now, w, h, font, snap, combat, chars}
  overlay.cpp: D3D11 Present hook, ImGui, Insert menu, runs every feature, calls Off() on toggle-off and unload
  draw.hpp: FormatAmount, OutlinedText
features/<kebab>/:   # one player-facing feature per folder; CMake globs features/*/*.cpp and features/*/*/*.cpp (not test/)
  <kebab>.cpp: one static Feature object; registers itself
  <kebab>.hpp + test/<kebab>_test.cpp: SDK-free logic + assert test (picked up by `just test`)
features/<domain>/:  # features of one domain + their shared code (new-feature.sh <domain>/<kebab>)
  shared/: domain code, one responsibility per file (inventory/shared: model.hpp record, order.hpp profiles+order, io.hpp read api → names/spec/read/probe.cpp; items.hpp includes all three); test/ picked up by `just test`
  <kebab>/: one feature as above; includes "../shared/…", never a sibling feature
```

## Rules and why
- Render thread (`OnFrame`) does memory reads/writes only, no `ProcessEvent`/UFunction calls: UE isn't thread-safe there and it crashes intermittently.
- Game-thread hooks (ProcessEvent) record and return fast; hand data to the render thread under an `SRWLOCK` (never `std::mutex`, see gotchas).
- Check every pointer (`PtrOk`) and `IsA` before casting: the same objects are different classes in menus/lobby. A pointer kept past the call goes into `core/ref.hpp`: actors die between frames and Blueprint classes with the map (#63).
- Movement/stats are server-simulated: apply to every player character; co-op only works when the host runs the pack.
- `Off()` restores captured originals, only on objects found in the live world (stale pointers may be freed).
- Logic with branches/math goes into an SDK-free header with a test: the in-game loop can't be unit-tested.
- Public repo: no SDK, no personal email (commit as the account's GitHub noreply), no absolute home paths.
