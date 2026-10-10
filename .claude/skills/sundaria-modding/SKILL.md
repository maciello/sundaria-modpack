---
name: sundaria-modding
description: Main entrypoint for ALL work on Dungeons of Sundaria (UE 4.27, Steam 587520) and the sundaria-modpack repo — the dos-tool DLL (ImGui overlay, damage numbers, DPS meter, camera, air control, health bars), the Dumper-7 SDK, hot reload into the running game, crash dumps, and the GitHub-release auto-updater friends use. Use whenever the user mentions Sundaria, the modpack, dos-tool, a new in-game feature/tweak (UI, numbers, colours, design system, visuals, movement, camera, stats), a game crash, "the game patched and the mod broke", re-dumping the SDK, releasing to friends, finding a game class/field/offset, or asking how the game works (who calls a function, who writes a variable, what an event triggers, live values in the running game) — even if they don't say "mod".
---

# Sundaria modding: router

**The reader is also the maintainer of this skill.** Before working, load `skill-creator:skill-creator`
(Skill tool) and follow its writing guide whenever you change anything here.

Maintenance contract:
- This file only routes. New knowledge goes into a new or existing `references/*.md`, a new `scripts/` tool,
  or an `assets/` template, plus one row in the tables below. Never grow a section here.
- Learned something the hard way (crash, wrong assumption, workaround)? Add it to `references/gotchas.md` in the same commit as the fix.
- Found out how the game works (a `just data` / `just game` answer, a probe result)? Add it to `references/game-facts.md` in the same ship: what, where (class::function @offset / table), the command that shows it, verified or not. Facts and locations only, never copied pak data. A finding left only in a report or issue is lost.
- A step you ran by hand twice becomes a script here.
- Keep this file under ~60 lines.

## Ask the game before guessing
Unsure how the game works? Look it up first (≤1 s each); guesses + probe builds cost hours.
- game logic (who calls / writes / reads X, what an event triggers, why our hook never sees a call): `just data callers|writers|readers|calls|events|ast <name>`, offline
- running game (live values, nearby actors, what fires now, the screen): `just game get|find|call|trace|shot|log|feature`, dev install

## Route by task
| task | read / run |
|---|---|
| where code goes, rules (threads, pointers, co-op, public repo) | `references/architecture.md` |
| add a feature, find game data, dev loop, release, patch | `references/workflows.md` |
| react to a game event (a function call, the world tick, a widget's calls), no polling: `game::On` / `OnWorldTick` / `OnClass` / `OnGameTick` | `references/game-events.md` |
| any game object: paths, offsets, verified or not, combat-log, damage types | `references/game-facts.md` |
| build/compile errors, hot-reload internals, SDK re-dump, updater | `references/toolchain.md` |
| something fails or crashed | game crashed: `just crashes` first (every UE crash dump, newest first, fault site), then `scripts/minidump.py <dmp>` and `references/gotchas.md`; the mod log `dos-tool.log` `[crash maybe]` lines add the stack |
| a question about the RUNNING game (live values, actors nearby, what fires, how it looks now): `just game get/find/call/trace/shot/log`; switch a feature on/off without the Insert menu: `just game feature` | `references/live-bridge.md` |
| new machine (SDK, MSVC kit), working together, trunk/flags | repo `CLAUDE.md` → `just sdk-pull` |
| extend a game screen (inventory, bank, menus): its UMG widgets, styles, how to add ours; `just ui <Class>` dumps live trees | `references/game-ui.md` |
| an effect in the world (sparkle, glow, marker on an actor or spot): game particle templates + `core/fx.hpp` | `references/fx.md` (`just data fx <regex>`) |
| any visual: colours, sizes, motion, icons, component specs (numbers, bars, loot, cards, menu) | `references/design-system.md` + tokens `core/style.hpp`; rule `.claude/rules/design.md`; reference images (local only, never commit) `../design-refs/<topic>/index.yaml` |
| DPS of a build, item scores (ΔDPS%), best in slot, stat weights; in game or offline `just dps` | `references/dps-library.md` (`libs/dps`) |
| ability scripts: Lua host, ECS, new actions/events | `references/ability-mods.md` |
| custom animations on the player (Paragon → skeleton copy → retarget → ~mods pak), skeleton facts | `references/animation-pipeline.md` |
| new work, idea, bug: file / claim / close an issue | `.claude/rules/issues.md` |

## Tools
| path | does |
|---|---|
| `scripts/sdk.py` | query the SDK: `class`, `chain`, `field <regex>`, `subs` |
| `just data` (repo `scripts/data.py`) | offline pak data: `find`, `show` JSON, `table` YAML, `bp` bytecode, `grep` values; Blueprint xref index: `callers`, `writers`/`readers`, `calls`, `events`, `ast`; `fx` particle templates → `references/game-data.md`, `references/fx.md` |
| `just game` (repo `scripts/game.py`) | live game over 127.0.0.1 (dev install): `get` path, `find` actors, `call`, `trace` ProcessEvent, `shot` PNG, `log`, `feature` list/toggle → `references/live-bridge.md` |
| `scripts/new-feature.sh` | scaffold `features/<kebab>/` (+ `--logic`: header + test) |
| `scripts/dev-check.sh` | game alive? hot-swap happened? log tail |
| `scripts/minidump.py` | crash dump (default: newest in the Proton prefix `…/Archon/Saved/Crashes/UE4CC-*/UE4Minidump.dmp`) → fault module+offset + stack, ours as function file:line (`--dll` for a rebuild) |
| `scripts/skeleton_fbx.py`, `retarget.py`, `retarget_map.py`, `helper_model.py` | animation pipeline (Blender headless): bone dump → FBX parts, retarget, helper-bone rules |
| `assets/feature.cpp.tmpl`, `logic.hpp.tmpl`, `logic_test.cpp.tmpl` | templates used by new-feature.sh |
