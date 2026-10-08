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
