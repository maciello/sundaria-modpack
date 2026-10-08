#!/usr/bin/env bash
# dev-check.sh — after `just dev`: is the game running, did the loader swap the dll, what did it log.
W=${GAME_WIN64:-$HOME/.steam/steam/steamapps/common/DungeonsofSundaria/Archon/Binaries/Win64}
pgrep -f '[A]rchon-Win64-Shipping' >/dev/null && echo "game: running" || echo "game: not running"
[[ -e $W/dos-tool.dev ]] && echo "dev flag: on (hot reload)" || echo "dev flag: off (load once) — run just dev-install with the game closed"
ls -l --time-style=+%T "$W"/DoS-Tool*.dll 2>/dev/null   # newest loaded<N>.dll size == DoS-Tool.dll size ⇒ swapped
echo "--- dos-tool.log"; tail -n 12 "$W/dos-tool.log" 2>/dev/null
