---
paths:
  - "mods/**"
---
# Mod code
- New feature = new folder under `features/`. Never grow another feature's file to host it.
- A feature talks to the game only through `core/game.hpp` or its own SDK-including `.cpp`. Only `.cpp` files include the SDK.
- Features do not include each other. Shared needs move into `core/`.
- Render thread (`OnFrame`, Present hook): memory reads/writes only. No `ProcessEvent` / UFunction calls.
- Check every pointer (`PtrOk`) and `IsA` before casting.
- Server-simulated changes (movement, stats) apply to every player character, not only the local one.
- Every feature is toggleable (Insert menu); `Off()` restores the captured vanilla values.
- Logic with branches/math lives in an SDK-free header with `test/*_test.cpp` next to it.
- Never walk `GObjects` to find game objects. Reach them through their owner (local controller → components, a widget → its fields) or from the game's own events (`game::SetEventListener`). The only exceptions: dev probes behind a file trigger, and a one-time singleton lookup in `core/`, cached, with its cost in ms logged.
- Per-frame and per-event cost is O(1) or O(what is on screen), never O(objects in the world). Work runs when an event says something changed, not on a timer.
- Shared domain code (items, containers, inventory widgets) lives in `core/<domain>.{hpp,cpp}`, one responsibility per file. A file growing past ~300 lines gets split before anything is added to it. Features stay thin: wiring + their own UI.
- One step per commit: move, then change behaviour, then optimise. Never in the same commit.
