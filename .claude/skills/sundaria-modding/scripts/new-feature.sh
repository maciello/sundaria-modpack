#!/usr/bin/env bash
# new-feature.sh [<domain>/]<kebab-name> "<Menu Title>" [--logic]
# Scaffolds mods/dos-tool/features/[<domain>/]<name>/<name>.cpp (CMake globs it); --logic adds an SDK-free
# <name>.hpp + test/<name>_test.cpp that `just test` picks up. Refuses to overwrite.
set -euo pipefail
path=${1:?kebab-name}; name=$(basename "$path"); title=${2:?"Menu Title"}
here=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)  # resolve the ~/.claude/skills symlink to the repo copy
dir=$(cd "$here/../../.." && pwd)/mods/dos-tool/features/$path
[[ -e $dir ]] && { echo "exists: $dir" >&2; exit 1; }
type=$(echo "$name" | sed -E 's/(^|-)([a-z])/\U\2/g'); var=${name//-/_}
sub() { sed -e "s/__TITLE__/$title/g" -e "s/__TYPE__/$type/g" -e "s/__VAR__/$var/g" -e "s/__NS__/$var/g" -e "s/__NAME__/$name/g" "$1"; }
mkdir -p "$dir"
sub "$here/assets/feature.cpp.tmpl" > "$dir/$name.cpp"
if [[ ${3:-} == --logic ]]; then
  mkdir -p "$dir/test"
  sub "$here/assets/logic.hpp.tmpl" > "$dir/$name.hpp"
  sub "$here/assets/logic_test.cpp.tmpl" > "$dir/test/${name}_test.cpp"
fi
find "$dir" -type f
