# Workflows (commands run from pack/ unless noted; scripts are relative to the skill dir)

## New feature
`scripts/new-feature.sh <kebab> "<Menu Title>" [--logic]` scaffolds from `assets/`. Then: find the game data
(below), add the read/write to `core/game.*`, logic + test in the feature's header, drawing/applying in
`OnFrame`, restore vanilla in `Off`. Then the dev loop.

## Find game data — never grep the 140 MB SDK by hand
```bash
scripts/sdk.py field 'Health|AirControl'   # owner::member @offset (type) file:line
scripts/sdk.py class UCharacterMovementComponent
scripts/sdk.py chain AArchonCharacter      # → ACharacter → APawn → AActor → UObject
scripts/sdk.py subs UDamageType            # what IsA() will catch
```
Read `game-facts.md` first; add every newly confirmed path there.

## Dev loop
```bash
just test               # every host test + updater
just dev                # build + hot-swap into the running game (~1 s); first time: just dev-install with game closed
scripts/dev-check.sh    # game running? swap happened? log tail
scripts/minidump.py     # game crashed: fault module+offset + stack (newest UE4Minidump.dmp)
```
A swap is verified only when dev-check shows the newest `loaded<N>.dll` size equals `DoS-Tool.dll` and the log
re-printed `ImGui (D3D11) initialised`. How it looks in game is the user's call: say "unverified visually" until
they confirm. A new hook can crash the user's game: ship it default-off and let the user enable it.

## Push
Only `just ship` (pull --rebase origin master + push HEAD:master; works from any worktree). Never `git push` by hand.
The pre-push hook runs `just test` on a clean checkout of the pushed commit, so uncommitted edits can't fake a pass.
Parallel work: one `git worktree` per task (`../wt-<task>`); set work aside with a WIP commit, never `git stash`
(the stash is shared by every worktree and session).

## Ship to friends
`just release` (version computed from commits, rule `.claude/rules/versioning.md`; `just next-version` = dry run; test → dist zip rooted at the game dir → GitHub release); friends' updater installs it on
next launch. Ask the user first: it reaches other people's machines.

## Game patched
Re-dump → rebuild → release: `toolchain.md` § Re-dump.
