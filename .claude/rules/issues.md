# Issues = the backlog
Every change starts as a GitHub issue — whether a player, a developer or an agent asked for it. File it before writing code, so anyone (human or agent, zero context) can pick it up from the issue alone.

## Hierarchy (native sub-issues: `gh issue create --parent <N>`)
| type | is | parent |
|---|---|---|
| Epic | player goal spanning several mods (e.g. "cast indicator") | — |
| Feature | one mod / one capability = one folder under `features/` | Epic, if any |
| Task | one commit-sized step of a Feature | Feature |
| Bug | something behaves wrong (game or mod) | the Feature it breaks, else none |

Labels: `type:epic|feature|task|bug`, `mod:<folder>` (`just labels` creates one per folder), `prio:P0..P3`. A feature's stage (Alpha → Stable) lives in code, never in labels.

## Body (forms in `.github/ISSUE_TEMPLATE/`; agents write the same headings)
- **Source**: who asked, verbatim, and where (chat date, issue/comment link). Quote; a paraphrase alone loses the requirement.
- **Story**: As a <player|developer>, I want <capability>, so that <outcome>.
- **Acceptance criteria**: Given/When/Then, each checkable by a named test, log line or screenshot.
- **Context**: files, SDK classes + offsets, skill refs (`game-facts.md` …), related issues, evidence (log excerpts). Mark unverified facts `unverified`.
- **Out of scope**, **Unknowns/risks**, **Size** (S/M/L).
- **Done when**: `just test` green, acceptance criteria verified in-game, skill updated with what was learned, stage set.

## Working an issue
- Claim: assign yourself (`gh issue edit <N> --add-assignee @me`) before starting; one owner per issue.
- Findings go into issue comments as you learn them (what was tried, evidence), not only into your head or chat.
- Close from master: commit message `Fixes #N`. Split work that grows: new Task sub-issues, not a longer issue.
- Public repo: no SDK dumps, home paths, hostnames, emails or tokens in issues.
