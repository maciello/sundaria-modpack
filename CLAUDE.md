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

## Working together (trunk-based)
- One branch: `master`. Pull with rebase, commit small, push at least daily. No long-lived branches.
- `git pull --rebase` before push. `git config pull.rebase true` once per clone.
- `just test` must pass before push; CI (`.github/workflows/test.yml`) re-runs it on every push. Red master = fix or revert first.
- Unfinished work ships dark: new feature constructed `Feature("Name (wip)", false)`. Drop `(wip)` and default `true` when done.
- Two people on the same feature: one owns the folder, the other sends changes as small commits or a PR.
- `core/` changes affect every feature: keep them small and push them on their own.
- Claim work in a GitHub issue before starting, so nobody builds the same thing twice.

## SDK and toolchain (not in repo)
- Shared dump store: `/srv/dumps/<game>/<steam-buildid>/` on the dump host (ssh alias `dumps`). Sundaria: `/srv/dumps/sundaria/<buildid>/CppSDK`.
- Steam build id: `steamapps/appmanifest_587520.acf` → `buildid`. SDK must match the game's build id.
- `just sdk-pull` copies the matching SDK to `../sdk/CppSDK`. On the dump host itself: `SDK_DIR=/srv/dumps/sundaria/<buildid>/CppSDK`.
- MSVC kit: `/srv/toolchains/xwin-msvc` on the dump host; `XWIN=` that path, or rsync it to `../tools/msvc`.

## Dev loop
`just dev-install` once with the game closed, then `just dev` hot-reloads `DoS-Tool.dll` into the running game.
`just test` before every commit. `just release vX.Y.Z` publishes; friends' updaters pick it up on next launch.

## Start here
Skill `sundaria-modding` (`.claude/skills/sundaria-modding/`) is the entrypoint for all work on this game:
it routes to architecture, workflows, game facts, gotchas and tools. Whoever reads it maintains it.
