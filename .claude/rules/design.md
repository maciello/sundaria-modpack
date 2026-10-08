# Design (every visual)
Visual = anything drawn, animated or framed: overlay text/shapes, icons, glow, particles, HUD, menu styling, camera moves and cutscenes.
- Who: visual work is designed AND implemented by an Opus agent (maintainer, 2026-10-08: "all visuals by opus"). Sonnet agents do only the non-visual parts (data, SDK reads, logic, filing) and hand drawing to Opus.
- Tokens: colours, sizes, spacing, radii, strokes, easing, durations and layers come from `mods/dos-tool/core/style.hpp`. No literal RGB in feature code; component-only geometry from the spec goes into a named constexpr in the feature's header. A value two features share becomes a token there (with a `style_test.cpp` assertion when it is an invariant).
- Specs: components follow `.claude/skills/sundaria-modding/references/design-system.md` (anatomy, sizes, tokens, motion, states, do/don't).
- Not covered: label the GitHub issue `design`, note "implementer: Opus", comment what is needed, and wait. The design lead answers with a "Design spec" comment and adds it to design-system.md; build from that.
- Done: a screenshot or clip on the issue; until the maintainer has seen it, say "unverified visually".
