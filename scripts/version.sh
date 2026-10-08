#!/bin/sh
# SemVer for releases: `version.sh next` | `notes` | `check [tag]`. Rule: .claude/rules/versioning.md
set -eu
RE='^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'
last() { git tag -l 'v*' | sort -V | tail -1; }

check() {
  for t in ${1:-$(git tag -l 'v*')}; do
    echo "$t" | grep -Eq "$RE" || { echo "malformed version tag: $t (want vX.Y.Z)"; exit 1; }
  done
}

next() {
  check
  l=$(last); l=${l:-v0.0.0}; range=""; [ "$l" = v0.0.0 ] || range="$l..HEAD"
  IFS=. read -r M m p <<EOF2
${l#v}
EOF2
  # breaking: `area!:` subject or `BREAKING CHANGE:` body line
  if git log $range --format='%s%n%b' | grep -Eq '^[a-z0-9_-]+(\([^)]*\))?!:|^BREAKING CHANGE:'; then
    if [ "$M" -eq 0 ]; then m=$((m + 1)); p=0; else M=$((M + 1)); m=0; p=0; fi   # 0.x: breaking = minor
  elif git log $range --format=%s | grep -Eq '^feat(\([^)]*\))?:' ||
       { [ -n "$range" ] && git diff --diff-filter=A --name-only "$l" HEAD -- 'mods/*/features/*/*.cpp' |
           while read -r f; do d=$(dirname "$f"); git cat-file -e "$l:$d" 2>/dev/null || echo new; done | grep -q new; }; then
    m=$((m + 1)); p=0
  else
    p=$((p + 1))
  fi
  echo "v$M.$m.$p"
}

notes() {
  l=$(last); range=""; [ -z "$l" ] || range="$l..HEAD"
  git log $range --no-merges --format='- %s'
}

case "${1:-}" in
  next) next ;;
  notes) notes ;;
  check) shift; check "$*" ;;
  *) echo "usage: version.sh next|notes|check [tag]"; exit 2 ;;
esac
