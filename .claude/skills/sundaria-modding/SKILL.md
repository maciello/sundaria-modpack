---
name: sundaria-modding
description: Main entrypoint for ALL work on Dungeons of Sundaria (UE 4.27, Steam 587520) and the sundaria-modpack repo — the dos-tool DLL (ImGui overlay, damage numbers, DPS meter, camera, air control, health bars), the Dumper-7 SDK, hot reload into the running game, crash dumps, and the GitHub-release auto-updater friends use. Use whenever the user mentions Sundaria, the modpack, dos-tool, a new in-game feature/tweak (UI, numbers, colours, movement, camera, stats), a game crash, "the game patched and the mod broke", re-dumping the SDK, releasing to friends, or finding a game class/field/offset — even if they don't say "mod".
---

# Sundaria modding: router

**The reader is also the maintainer of this skill.** Before working, load `skill-creator:skill-creator`
(Skill tool) and follow its writing guide whenever you change anything here.

Maintenance contract:
- This file only routes. New knowledge goes into a new or existing `references/*.md`, a new `scripts/` tool,
  or an `assets/` template, plus one row in the tables below. Never grow a section here.
- Learned something the hard way (crash, wrong assumption, workaround)? Add it to `references/gotchas.md` in the same commit as the fix.
- A step you ran by hand twice becomes a script here.
- Keep this file under ~60 lines.

## Route by task
| task | read / run |
|---|---|
| where code goes, rules (threads, pointers, co-op, public repo) | `references/architecture.md` |
| add a feature, find game data, dev loop, release, patch | `references/workflows.md` |
| any game object: paths, offsets, verified or not, combat-log, damage types | `references/game-facts.md` |
| build/compile errors, hot-reload internals, SDK re-dump, updater | `references/toolchain.md` |
| something fails or crashed | `references/gotchas.md` |
| new machine (SDK, MSVC kit), working together, trunk/flags | repo `CLAUDE.md` → `just sdk-pull` |

## Tools
| path | does |
|---|---|
| `scripts/sdk.py` | query the SDK: `class`, `chain`, `field <regex>`, `subs` |
| `scripts/new-feature.sh` | scaffold `features/<kebab>/` (+ `--logic`: header + test) |
| `scripts/dev-check.sh` | game alive? hot-swap happened? log tail |
| `scripts/minidump.py` | crash dump → fault module+offset + stack |
| `assets/feature.cpp.tmpl`, `logic.hpp.tmpl`, `logic_test.cpp.tmpl` | templates used by new-feature.sh |
