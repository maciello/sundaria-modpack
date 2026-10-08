# Versioning (releases)
- SemVer 2.0.0, tags `vX.Y.Z` only (no `-rc`, no build metadata). `just test` fails on any other `v*` tag.
- Never choose a version: `just release` computes it (`just next-version` = dry run: version + notes). Never `git tag` / `gh release create` by hand.
- Bump from commits since the last tag, highest wins:
  - breaking: subject `<area>!: ...` or a `BREAKING CHANGE:` body line. Major; while major is 0 only minor.
  - minor: a new folder under `features/` (new feature), or subject `feat: ...`.
  - patch: everything else.
- Subjects keep the `<area>: <change>` style; they become the release notes verbatim, so write them for players.
- Release only from a clean tree at origin/master; ask the maintainer first (`workflows.md` § Ship to friends).
- Updater: installs the latest GitHub release by tag equality with `.modpack-version`; no ordering is compared, so no SemVer parsing there.
