# sundaria-modpack — conventions

## Layout
```
mods/<mod>/                  one shippable mod (today: dos-tool = one .asi loader + one .dll)
  core/                      host: loader, hooks, overlay/menu shell, SDK access, shared services
    test/                    native tests for SDK-free core code
  features/<feature>/        ONE player-facing feature per folder (kebab-case)
    <feature>.cpp            one static `feature::Feature` (core/feature.hpp): name, enabled, OnFrame(), Menu(), Off()
    *.hpp                    SDK-free logic (testable natively)
    test/*_test.cpp          native test of that logic; `just test` runs every one
  assets/                    fonts etc. + their licence files
updater/                     launch-option updater (py + ps1), shared by all mods
```

## Rules
- New feature = new folder under `features/`. Never grow another feature's file to host it.
- A feature talks to the game only through `core/game.hpp` (shared, once-per-frame reads) or its own SDK-including `.cpp`. Only `.cpp` files include the SDK.
- Features do not include each other. Shared needs (e.g. combat events used by damage numbers and DPS meter) move into `core/`.
- Render-thread code reads/writes memory only. No `ProcessEvent` / UFunction calls from the Present hook.
- Gameplay changes that the server simulates (movement, stats) apply to every player character, not just the local one, so co-op stays in sync when everyone runs the pack.
- Every feature is toggleable in the menu (Insert) and defaults to the vanilla behaviour when off (restore originals).
- Logic with branches/math lives in an SDK-free header with a test next to it.
- Game patch → re-dump SDK (work item `just dump-install`, End in a dungeon, `just dump-collect`) → rebuild → release.

## Dev loop
`just dev-install` once with the game closed, then `just dev` hot-reloads `DoS-Tool.dll` into the running game.
`just test` before every commit. `just release vX.Y.Z` publishes; friends' updaters pick it up on next launch.
