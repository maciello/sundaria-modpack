#!/bin/sh
# Fails on any level-actor walk (`Levels.Num()`) in mods/dos-tool outside the file:function list below. Every listed
# function runs on the game thread (a game::SetEventListener listener behind game::OnGameThread, or core's CoreTick);
# the render thread (OnFrame, Menu, Off, the Present hook) never walks or dereferences world actors: the game thread
# destroys them meanwhile (#79 crash, audit #80). New walk: put it on the game thread, then add it here.
# Rule: .claude/rules/mod-code.md.
set -eu
ALLOW='core/game.cpp:SampleNow core/game.cpp:ForEachPlayerMovement core/game.cpp:BuildRoom core/game.cpp:CollisionTick
core/game.cpp:ForEachActor core/game.cpp:LogActorsNow
features/hub/shared/hub_ui.cpp:ForEachButton features/hub/shared/map_probe.cpp:Survey features/hub/shared/mini_map.cpp:ForEachActor
features/hub/shared/props.cpp:Find features/hub/shared/props.cpp:NearNow features/hub/shared/npc_audio.cpp:Apply
features/loot/shared/track.cpp:Scan features/inventory/dps-probe/dps-probe.cpp:Enemies'
cd mods/dos-tool
bad=$(grep -rn --include='*.cpp' --include='*.hpp' 'Levels\.Num()' core features | grep -v '/test/' |
  while IFS=: read -r f n _; do
    fn=$(head -n "$n" "$f" | grep -E '^ *[A-Za-z_][A-Za-z0-9_:<>*&, ]* [*&]?[A-Za-z_][A-Za-z0-9_:]*\(.*\) *(const *)?(override *)?\{ *(//.*)?$' | tail -1 | sed -E 's/\(.*//; s/.*[ *&:]//')
    echo " $ALLOW " | tr '\n' ' ' | grep -q " $f:$fn " || echo "$f:$n ($fn)"
  done)
[ -z "$bad" ] || { echo "level-actor walk outside the game-thread allowlist (scripts/actor-walk-check.sh):"; echo "$bad"; exit 1; }
