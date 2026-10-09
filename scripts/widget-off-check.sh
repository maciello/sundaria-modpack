#!/bin/sh
# Fails when a folder under mods/dos-tool/features spawns widgets (umg::Spawn, WidgetBlueprintLibrary::Create,
# AddToViewport, AddChild) or particle components (fx::Attach / fx::At, or SpawnEmitter* / SpawnSystem* directly) but
# no file in it removes them through a world tick: game::Drain (core/drain.hpp) or fx::Release (core/fx.hpp). Else
# they stay frozen in the world after a hot reload or toggle-off (#84, #86, #110). Rule: .claude/rules/mod-code.md.
set -eu
cd mods/dos-tool/features
bad=$(grep -rlE 'umg::Spawn\(|WidgetBlueprintLibrary::StaticClass|"AddToViewport"|"AddChild"|"SpawnEmitter|"SpawnSystem|fx::Attach\(|fx::At\(' --include='*.cpp' . | grep -v '/test/' |
  while read -r f; do
    d=$(dirname "$f")
    grep -qsE 'game::Drain|fx::Release\(' "$d"/*.cpp || echo "$f"
  done)
[ -z "$bad" ] || { echo "spawner without game::Drain / fx::Release in its folder (scripts/widget-off-check.sh):"; echo "$bad"; exit 1; }
