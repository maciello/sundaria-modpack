# Working together (trunk-based)
- One branch: `master`. Commit small, push at least daily. No long-lived branches.
- `git config pull.rebase true` once per clone; `git pull` before every push.
- `just test` passes before push. CI (`.github/workflows/test.yml`) re-runs it on every push. Red master = fix or revert first.
- Unfinished work ships dark: construct the feature as `Feature("Name (wip)", false)`. Drop `(wip)` and default `true` when done.
- Claim work in a GitHub issue before starting, so nobody builds the same thing twice.
- A feature folder has one owner at a time; others send small commits or a PR.
- `core/` changes affect every feature: keep them small and push them as their own commit.
