# Contributing

## First time
1. Game must run under Proton: Steam → Dungeons of Sundaria → Properties → Compatibility → force Proton, let Steam download the Windows build (`Archon/Binaries/Win64/` appears).
2. Install: `clang lld llvm cmake just rsync` (Fedora: `sudo dnf install clang lld llvm cmake just rsync`).
3. `~/.ssh/config`: a `Host dumps` entry for the dump host (ask the repo owner). Not needed on the dump host itself.
4. Clone, then once:
   ```bash
   just setup         # pull = rebase, tests run before every push
   just build         # first run fetches the game SDK + MSVC kit (~760 MB)
   just dev-install   # game closed: installs the hot-reload loader
   ```

## Every day
| when | command |
|---|---|
| start | `just sync` — get the others' commits |
| game running, code changed | `just dev` — new DLL is live in ~1 s |
| stop | `just ship` — test, rebase, push |

Menu in game: **Insert**.

## Rules
- One branch (`master`), small commits, push daily. Claim work in a GitHub issue first.
- New feature: `.claude/skills/sundaria-modding/scripts/new-feature.sh <kebab> "<Title>"` → starts as `Alpha`.
- Stages: `Alpha` (only with `dos-tool.dev`) → `Beta` (off by default) → `Stable` (on) → `Deprecated`. Your on/off choices live in `dos-tool.ini` in the game folder, not in git.
- Full rules: [`.claude/rules/`](.claude/rules/). Claude Code loads them by itself.

## Game patch
The SDK must match the game's Steam build id. After a patch: re-dump with Dumper-7, upload to the dump store under the new build id, `just build`, `just release vX.Y.Z`.
