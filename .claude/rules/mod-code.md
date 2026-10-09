---
paths:
  - "mods/**"
---
# Mod code
- New feature = new folder under `features/`. Never grow another feature's file to host it.
- A feature talks to the game only through `core/game.hpp` or its own SDK-including `.cpp`. Only `.cpp` files include the SDK.
- Features do not include each other. Shared needs move into their domain's `shared/`, or `core/` if domain-free.
- Render thread (`OnFrame`, Present hook): memory reads only. No `ProcessEvent` / UFunction calls.
- Render thread never walks or dereferences world actors (level actor lists, actor `TArray`s, actors/components/attribute sets kept across frames) and never writes game memory: the game thread does (core `CoreTick`, or a listener behind `game::OnGameThread()`) and hands plain data over under SRWLOCK; the game thread frees actors meanwhile (#79, #80). Fresh reads down the local controller → pawn / camera manager chain are fine. `just test` enforces the walks (`scripts/actor-walk-check.sh`, allowlist by file:function).
- Every engine pointer kept beyond the current call (UFunction, UClass, UObject, actor, widget …⊇) is stored only in `core/ref.hpp` (`ref::Ref` / `Cached` / `Fn`) and validated by GObjects index + name on each use (O(1)): map travel frees Blueprint classes and world objects and reuses their memory (#63). `PtrOk` is for null and garbage only, never for liveness. `IsA` before casting. `just test` enforces this (`scripts/ref-check.py`, allowlist by file:symbol).
- Server-simulated changes (movement, stats) apply to every player character, not only the local one.
- Every feature is toggleable (Insert menu); `Off()` restores the captured vanilla values.
- Logic with branches/math lives in an SDK-free header with `test/*_test.cpp` next to it.
- Never walk `GObjects` to find game objects. Reach them through their owner (local controller → components, a widget → its fields) or from the game's own events (`game::On` / `OnWorldTick` / `OnClass`, skill `references/game-events.md`). The only exceptions: dev probes behind a file trigger, and a one-time singleton lookup in `core/`, cached, with its cost in ms logged. `just test` enforces this (`scripts/gobjects-check.sh`, probes allowlisted by file:function).
- Per-frame and per-event cost is O(1) or O(what is on screen), never O(objects in the world). Work runs when an event says something changed, not on a timer.
- Features of one domain share a parent: `features/<domain>/{shared,<feature>…}` (e.g. `features/inventory/{shared,item-sort,item-sell}`). Domain code (items, containers, inventory widgets) lives in its `shared/`, one responsibility per file; `core/` holds only domain-free host code. A file growing past ~300 lines gets split before anything is added to it. Features stay thin: wiring + their own UI.
- One step per commit: move, then change behaviour, then optimise. Never in the same commit.
- Core does per-frame or per-tick work only while an enabled feature needs it (e.g. `Feature::usesCombat` gates character sampling). With every feature off, the mod costs nothing and touches nothing in the world.
- `Off()` runs with the game-thread hook alive (unload: Present/Resize unhooked first, `kiero::shutdown()` after all `Off()`s). Widgets/actors we spawned are removed in `Off()`: hand it to a world tick with `game::Drain` (`core/drain.hpp`: `Serve` in the listener on the world tick, `Request` in `Off()`: bounded wait + fallback), never leave them "until the game clears" (#84). `just test` enforces it (`scripts/widget-off-check.sh`: a feature folder that spawns widgets must use `game::Drain`).
