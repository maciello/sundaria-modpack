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
libs/<lib>/                   SDK-free C++ shared by the DLL and offline hosts: include/ (API), src/ (in the DLL), host/ (offline only), test/
```

## Rules
`.claude/rules/`: versioning (SemVer, computed), issues (all work starts as a GitHub issue), collaboration (trunk-based, flags), public repo, SDK/toolchain, mod code (loaded for `mods/**`), design (every visual: tokens, specs, Opus-only).

## Dev loop
`just dev-install` once with the game closed, then `just dev` hot-reloads `DoS-Tool.dll` into the running game.
`just test` before every commit. `just release` publishes (version computed: `.claude/rules/versioning.md`); friends' updaters pick it up on next launch.

## Start here
Human onboarding: `CONTRIBUTING.md`. Skill `sundaria-modding` (`.claude/skills/sundaria-modding/`) is the entrypoint for all work on this game:
it routes to architecture, workflows, game facts, gotchas and tools. Whoever reads it maintains it.
