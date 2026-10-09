#!/bin/sh
# Fails when a folder under mods/dos-tool/features spawns widgets (umg::Spawn, WidgetBlueprintLibrary::Create,
# AddToViewport, AddChild) or particle components (SpawnEmitter*, SpawnSystem*) but no file in it uses game::Drain (core/drain.hpp): Off() must hand their removal to a
# world tick, else they stay frozen on screen after a hot reload or toggle-off (#84, #86).
# Rule: .claude/rules/mod-code.md.
set -eu
cd mods/dos-tool/features
bad=$(grep -rlE 'umg::Spawn\(|WidgetBlueprintLibrary::StaticClass|"AddToViewport"|"AddChild"|"SpawnEmitter|"SpawnSystem' --include='*.cpp' . | grep -v '/test/' |
  while read -r f; do
    d=$(dirname "$f")
    grep -qs 'game::Drain' "$d"/*.cpp || echo "$f"
  done)
[ -z "$bad" ] || { echo "widget spawner without game::Drain in its folder (scripts/widget-off-check.sh):"; echo "$bad"; exit 1; }
