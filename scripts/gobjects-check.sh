#!/bin/sh
# Fails on any GObjects walk (`GObjects->Num()`) in mods/dos-tool outside core/game.cpp, features/ui-probe/ and the
# file:function dev probes below. Rule: .claude/rules/mod-code.md (never walk GObjects).
set -eu
ALLOW='features/inventory/shared/probe.cpp:ContainersReport'
cd mods/dos-tool
bad=$(grep -rn --include='*.cpp' --include='*.hpp' 'GObjects->Num()' core features | grep -v '^core/game.cpp:' | grep -v '^features/ui-probe/' |
  while IFS=: read -r f n _; do
    fn=$(head -n "$n" "$f" | grep -E '^ *[A-Za-z_][A-Za-z0-9_:<>*& ]* [*&]?[A-Za-z_][A-Za-z0-9_:]*\(.*\) *(const *)?\{ *$' | tail -1 | sed -E 's/\(.*//; s/.*[ *&:]//')
    echo " $ALLOW " | grep -q " $f:$fn " || echo "$f:$n ($fn)"
  done)
[ -z "$bad" ] || { echo "GObjects walk outside core/game.cpp, ui-probe and the allowlisted probes:"; echo "$bad"; exit 1; }
