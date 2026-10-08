# Dungeons of Sundaria modpack

Mods for Dungeons of Sundaria (Steam 587520, UE 4.27.2). Install once; every game launch pulls the newest release.

| mod | what | source |
|---|---|---|
| dos-tool | damage numbers, camera FOV / distance / collision; menu: **Insert** | `mods/dos-tool` (fork of [RobUnderscore/sundaria-camera-fov](https://github.com/RobUnderscore/sundaria-camera-fov), MIT) |

## Install (once)

1. Download [`updater/update.py`](updater/update.py) (Linux) or [`updater/update.ps1`](updater/update.ps1) (Windows) and save it anywhere.
2. Steam → Dungeons of Sundaria → Properties → **Launch Options**:

   Linux / Steam Deck (the game must run with Proton: Properties → Compatibility → force Proton):
   ```
   WINEDLLOVERRIDES="winmm=n,b" python3 /path/to/update.py %command%
   ```
   Windows:
   ```
   powershell -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\path\to\update.ps1 %command%
   ```
3. Start the game. The updater installs/updates the pack into the game folder, then launches. No internet → the game starts with what is installed.

Uninstall: clear the launch option, delete the files listed in `.modpack-files` in the game folder.

## Game updates

Mods read game memory at offsets from a dump of one game build. A game patch can break them until a new release is out (re-dump with Dumper-7, rebuild).

## Develop

`just test` · `just build` (needs `SDK_DIR` = Dumper-7 CppSDK, `XWIN` = `xwin splat` output; neither is in this repo) · `just release vX.Y.Z`

Release zip paths are relative to the game root (`Archon/Binaries/Win64/...`, later `Archon/Content/Paks/~mods/...`).
