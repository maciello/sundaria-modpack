# Working together (trunk-based)
- One branch: `master`. Commit small, push at least daily. No long-lived branches.
- `just setup` once per clone (pull = rebase, `just test` runs before every push).
- `just sync` when you start (pull + last 10 commits). `just ship` when you stop (test → pull → push). Push rejected = run `just ship` again.
- CI (`.github/workflows/test.yml`) re-runs `just test` on every push. Red master = fix or revert first.
- Unfinished work ships dark: construct the feature as `Feature("Name (wip)", false)`. Drop `(wip)` and default `true` when done.
- Claim work in a GitHub issue before starting, so nobody builds the same thing twice.
- A feature folder has one owner at a time; others send small commits or a PR.
- `core/` changes affect every feature: keep them small and push them as their own commit.
