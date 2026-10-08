# Working together (trunk-based)
- One branch: `master`. Commit small, push at least daily. No long-lived branches. GitHub enforces it via `.github/rulesets/` (no force-push/delete, linear history, no other branches), applied by an admin with `just protect`.
- `just setup` once per clone (pull = rebase, `just test` runs before every push).
- `just sync` when you start (pull + last 10 commits). `just ship` when you stop (test → pull → push). Push rejected = run `just ship` again.
- CI (`.github/workflows/test.yml`) re-runs `just test` on every push. Red master = fix or revert first.
- Every feature has a stage (Kubernetes feature gates, `core/feature.hpp`): `Alpha` (new; off, shown only with `dos-tool.dev`) → `Beta` (works, needs testing; off, shown with BETA tag) → `Stable` (on by default) → `Deprecated` (off, about to go). Unfinished work ships as `Alpha`. With `dos-tool.dev` (developer install) every non-Deprecated feature defaults on, except `optIn` ones (debug probe).
- On/off choices are local per install (`dos-tool.ini` next to the game exe). Never commit them; change defaults only through the stage.
- Every change starts as a GitHub issue (epic/feature/task/bug, verbatim source, acceptance criteria): `.claude/rules/issues.md`. Claim it before starting, so nobody builds the same thing twice.
- A feature folder has one owner at a time; others send small commits or a PR.
- `core/` changes affect every feature: keep them small and push them as their own commit.
