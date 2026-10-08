# Design system

Tokens live in `mods/dos-tool/core/style.hpp` (SDK- and ImGui-free, asserted by `core/test/style_test.cpp`).
This file is the language and the component specs. Rule: `.claude/rules/design.md`. Missing a spec → label `design` on the issue.

Renderer: Dear ImGui 1.90.9 draw lists on the D3D11 overlay. No textures. Text = embedded display fonts
(`mods/dos-tool/assets`, Titan One default) baked at `type::kAtlasPx` 64 px: bigger text is upscaled and soft.
Colour to ImGui: `style::Pack(token, alpha)` == `IM_COL32`.

## Language
Genshin-inspired: clean outlined numerals, soft same-hue glows, crisp silhouettes, restrained motion.
Borderlands only for the boss name card (skewed slab, hard shadow, bold title).

| principle | rule |
|---|---|
| readable on anything | every glyph/text = fill + `color::kInk` outline at `stroke::kOutlineAlpha`; fills ≥ 4.5:1 on ink (tested) |
| one hue, one meaning | element colours only for elements, rarity only for loot, `kHpFill`/`kTaken` only for HP loss, `kGold` only for chrome (trim, menu, HUD accent). Never colour by size |
| size = magnitude | log scale, clamped (`type::kNumberMin..kNumberMax`), stacks capped at `kStackCap` |
| restrained motion | one primary motion per element; 0.12–0.6 s; overshoot (OutBack) only at birth; no looping motion except idle loot cues |
| soft glow | glow = same hue as the fill, wide, low alpha (`stroke::kGlowWidth`, `kGlowAlpha`); white only as an impact flash ≤ `motion::kFlash` |
| crisp silhouette | shapes from filled primitives; min stroke 1 px; min icon radius `type::kIconMinR` |
| scale | HUD: × `type::Ui(h)` (1080p = 1). World-anchored: × Ui × depth, depth = clamp(1500 cm / distance, 0.6, 1.25) |
| colour redundancy | colour is never the only channel: element = colour + icon, rarity = colour + beam/glow strength, suggestion = colour + glyph + text |

## Tokens (`style::`)
```yaml
color:   {kInk: ink outline 20,12,8, kShadow: drop shadow, kText: 255 white, kTextSoft: 235,225,205, kTextMuted, kGold: chrome accent,
          kPanel: 18,15,22 @.84, kPanelEdge: gold @.35, kTrack, kHpFill, kHpSheen, kHpChip, kHeal, kTaken, kGood: upgrade, kSell: sell}
element: kColor[combat::Element] / Of(e)   # Physical white, Fire, Ice, Lightning, Holy, Poison, Shadow, Arcane, Environment
rarity:  kTier[0..7] / Of(tier), kGlowFrom 3   # EItemGrade by rank, WoW quality colours; names + order unverified; the game's own GetItemColorForGrade wins once read
type:    {Ui(h), kXs 13, kSm 17, kMd 26, kLg 42, kXl 64, kAtlasPx 64, kNumberMin .8, kNumberMax 2.4, kStackCap 1.6, kHaloFrom 1.8,
          kCounterRatio .34, kCounterMinPx 12, kIconRatio .2, kIconMinR 6, kIconGlyph .68}   # px at 1080p
space:   {k1 2, k2 4, k3 6, k4 8, k5 12, k6 16, k7 24, k8 32}   # px at 1080p
radius:  {kSm 3, kMd 6, kLg 10, Pill(h) = h/2}
stroke:  {Outline(px) = max(1.5, px/16), kOutlineAlpha .85, kGlowWidth 3 (× Outline), kGlowAlpha .35, kShadowDy 1/24 (× px), kBarEdge 2}
ease:    Apply(Curve, x, k): Linear, OutCubic, InQuad, InOutCubic, OutBack(k overshoot)   # Penner closed forms
motion:  {kPop .22 OutBack 1.7, kRise .6 OutCubic, kBump .25 InQuad amp .35, kTick .12 OutCubic from 1.25, kFlash .12,
          kNumberLife 1.4, kFadeTail .3, kFadeIn .15, kFadeOut .4, kBarFlash .18, kChipHold .4, kDrainPerSec .8, kLinger 3,
          kPipFill .18 OutBack 2, kShimmerPeriod 2.4, kPulsePeriod 1.6, kCardIn .45, kCardOut .35, kCardHold 2.6}   # seconds
Layer:   WorldGlow < WorldBar (ImGui background list) < WorldNumber < Hud < Cinematic (foreground list) ; Menu = Insert window
```
Outline recipe: `draw::OutlinedText` 8 taps at `stroke::Outline(px)`. Glow: same call, fill alpha 0, outline = fill colour at
`kGlowAlpha`, width × `kGlowWidth`, drawn first. Drop shadow (cards only): text in `kShadow` offset (0, px·kShadowDy).

## Components

### Damage number
- Anatomy: amount (FormatAmount) · element icon top-right (non-Physical, non-Heal) · stack counter bottom-right (hits ≥ 2).
- Size: `kLg` × Ui × user size × `combat::Shown(n, cap)`; single hits use the full range, stacks cap at `kStackCap`; crits keep their size.
- Colours: Dealt = `element::Of(e)` (Physical always `kText`, no gold ramp); Heal = `kHeal` with "+"; Taken = `kTaken`. Impact flash mixes toward white for `kFlash` (+0.1 s × big).
- Big hit (Shown ≥ `kHaloFrom`): halo in the fill's own colour, alpha `kGlowAlpha` × big, for the number's whole life.
- Motion: pop `kPop` (k + 1.6·big) · rise `kRise` −1.6 units, drift ±0.9 · stack merge kick `kBump` amplitude 0.35 × (1 − 0.5·Shown/kNumberMax) · fade over the last `kFadeTail` of `kNumberLife`, InQuad, scale −25 %.
- Do: keep one number per ability stack. Don't: exceed `kAtlasPx` for stacks; colour by size; add looping motion.

### Stack counter
- "x<N>", N ≥ 2. Size max(`kCounterMinPx`·Ui, `kCounterRatio` × number px). Colour `kTextSoft` for every element. Same ink outline.
- Anchor: left = text right + `space::k1`·Ui; bottom = text box bottom. Owns the bottom-right; the icon owns the top-right.
- Motion: inherits; on each merge the counter alone punches `kTick` (1.25 → 1).

### Element icon
- Badge: disc `kInk` at `kOutlineAlpha`, radius R = max(`kIconMinR`, `kIconRatio` × number px), centre (text right + k1·Ui + R, text top + 0.6R).
- Glyph in `element::Of(e)`, unit r = `kIconGlyph`·R, coords in r, y down, centre (0,0), t = max(1, 0.22R):

| element | glyph | ImDrawList |
|---|---|---|
| Fire | flame | CircleFilled((0,.35), .62) + TriangleFilled((-.6,.2), (.6,.2), (.1,-1)) |
| Ice | snowflake | 3 × Line(−d, +d), d at 0°, 60°, 120°, thickness t |
| Lightning | bolt | TriangleFilled((.35,-1), (-.45,.15), (.1,.15)) + TriangleFilled((-.1,-.1), (.45,-.1), (-.35,1)) |
| Poison | droplet | CircleFilled((0,.3), .6) + TriangleFilled((-.52,.05), (.52,.05), (0,-1)) |
| Holy | sun | CircleFilled((0,0), .45) + 8 × Line(.62·d, 1.0·d) at k·45°, thickness t |
| Shadow | crescent | CircleFilled((0,0), 1) + CircleFilled((.45,-.3), .85, disc colour) |
| Arcane | sparkle | QuadFilled((0,-1), (.28,0), (0,1), (-.28,0)) + QuadFilled((-1,0), (0,-.28), (1,0), (0,.28)) |
| Environment | leaf | EllipseFilled((0,0), (.45,.95), rot −0.785) + Line((-.6,.6), (.55,-.55), disc colour, .6t) |

- Motion: none of its own. Don't: outline the glyph, drop the disc (crescent and leaf need it), show on Physical/Heal.

### Health bar (enemy, world-anchored)
- Anatomy (back → front): edge (rect +`kBarEdge`, `kInk` @.75) · track `kTrack` · chip `kHpChip` · fill `kHpFill` · sheen (top 45 %, `kHpSheen`) · level "Lv.N" left of the bar, gap `space::k3`.
- Size: w = 118 × Ui × width setting (0.6) × depth × pop; h = max(5, 8 × Ui × depth × pop); radius `Pill(h)`; pop = 0.92 + 0.08·alpha. Level text `kSm` × Ui × depth, min `kXs`.
- Motion: fade in `kFadeIn`, out `kFadeOut`; hit flash `kBarFlash` mixes fill 70 % toward white; chip holds `kChipHold` then drains `kDrainPerSec`; full-HP bar lingers `kLinger`.
- States: hidden until first hit; hidden while occluded; near bars on top (sort by distance). Boss bar: not specified (request it).

### Hit pips (cast indicator, #3)
- Anchor (HUD): row centred at (w/2, 0.62h), above the character's feet in the default camera.
- Pip: diamond, half-diagonal 6·Ui, edge gap `space::k2`·Ui. Empty: `kInk` @.55 + 1 px outline `kTextSoft` @.6. Filled: ability element colour (Physical `kText`) + glow disc radius 1.8× at `kGlowAlpha`.
- Cast timer (optional): 2·Ui line, `space::k2` below the row, row width, `kTextSoft` @.8 fill left → right.
- N > 10: one segmented bar (w = 120·Ui, h = 6·Ui, `Pill`) with N−1 ink ticks, segments fill like pips.
- Motion: row fades in `kFadeIn`; each hit: pip scales 0.4 → 1 with `kPipFill` + white flash `kFlash`; all filled: row punch 1.15 → 1 over 0.25 s OutCubic, hold 0.5 s, fade `kFadeOut`. Cast ended with empty pips: those turn `kTextMuted`, row fades. No red, no shake.

### Loot marker: rarity glow, beam, idle shimmer (#25, #27)
- Anchor: projected item position. depth as above; cull beyond 30 m, fade 25–30 m.
- Tier < `kGlowFrom`: ground ring only, alpha .5. Tier ≥ `kGlowFrom`: ring + glow. Tier ≥ 4: + beam. Tier ≥ 5: + drop burst.
- Ring: Ellipse radii (18, 6)·Ui·depth, stroke 2·Ui in tier colour @.8, filled @.18.
- Glow: 3 concentric CircleFilled r = (10, 16, 24)·Ui·depth, alpha .30/.15/.07, tier colour.
- Beam: RectFilledMultiColor, width 6·Ui·depth, height 90·Ui·depth up from the ring centre, bottom tier @.55 → top tier @0.
- Idle shimmer (unlooted only): white sparkle (Arcane glyph geometry, r = 7·Ui·depth) once per `kShimmerPeriod` at an offset inside the ring, scale 0 → 1 → 0 over 0.5 s InOutCubic; glow breathes alpha ×(0.75..1.0) sine at `kPulsePeriod`. Phase = hash(actor id) so markers never sync.
- Drop: ring + glow scale in with `kPop`; beam height 0 → full over 0.45 s OutCubic; burst = 8 radial lines, length 0 → 28·Ui, alpha 1 → 0 over 0.5 s, tier colour.
- Looted / emptied: everything fades `kFadeOut` (≤ 1 s).
- Don't: pulse scale (only alpha), draw through walls without fade, use element colours.

### Chest open burst (#24, overlay option A)
- On closed → open: flash disc r 40·Ui·depth `kTextSoft` @.5 → 0 over 0.25 s; 12 sparks = radial lines from r 0 → 60·Ui·depth (OutCubic, 0.6 s), length 10·Ui → 0; 6 motes (CircleFilled 2·Ui) rising 80·Ui over 1.2 s with drift ±10·Ui, alpha → 0.
- Colour: highest item tier in the chest if known, else `kTextSoft`. Particles deterministic from hash(actor id) (SDK-free, tested).

### Suggestion badge (#22, #23)
- Pill: height 18·Ui, padding x `space::k3`, `Pill(h)`, text `kXs`·Ui in `kInk`, no outline.
- Upgrade: fill `kGood`, glyph up-triangle (w 8, h 6 ·Ui) + "+<score delta>". Sell: fill `kSell`, glyph coin (CircleFilled r 4·Ui + Circle r 2.5·Ui in ink) + "SELL"; second line "superior: <name>" `kSm` `kTextSoft` (bank items add "(bank)").
- List row (overlay window): 3·Ui left bar in rarity colour, item name in rarity colour, badge right-aligned.
- Motion: badge pops `kPop` on first appearance only. Don't: animate lists; use red for sell.

### Boss name card (#19, Borderlands style)
- Cinematic layer: hides WorldNumber/Hud while shown. Letterbox: black rects top and bottom, 0 → 0.1h over 0.4 s InOutCubic, out the same.
- Plate: parallelogram skew 12°, anchor left edge at 0.08w, vertical centre 0.62h, height 96·Ui, width text + 2·`space::k8`·Ui, fill `kPanel`; `kGold` trim lines 3·Ui along top and bottom edges.
- Subtitle (epithet / FightStartedMessage) `kMd` `kTextSoft` above the name; name `kXl` `kText`, ink outline + hard drop shadow (offset (4, 4)·Ui, `kInk` @1).
- Motion: plate slides in from −40·Ui with `kCardIn`; name scales 1.3 → 1 over 0.2 s OutCubic after 0.1 s; hold `kCardHold`; out `kCardOut` slide +40·Ui + fade. Skip → all out in 0.1 s.
- Fallback: no name → class name with `BP_`/`_C` stripped. Names > 18 chars → `kLg`.

### Boss intro camera (#18)
- Sequence ≈ 4.6 s, skippable: approach 1.2 s InOutCubic → hold `kCardHold` with a linear 4°/s orbit → return 0.8 s InOutCubic to the live gameplay pose (re-read every frame).
- Framing: boss on the right third, eye level −10°, distance ≈ 2.5 × boss capsule height, FOV −10° vs gameplay. Letterbox + hidden HUD for the whole sequence.
- Don't: cuts, zoom punches, shake, roll. Borderlands energy stays in the name card.

### Menu (Insert window) and HUD panels
- ImGuiStyle: WindowRounding `kLg`, FrameRounding/GrabRounding `kMd`, WindowPadding (k6, k6), FramePadding (k4, k2), ItemSpacing (k4, k3); WindowBg `kPanel`; Border `kPanelEdge`; Text `kText`; TextDisabled `kTextMuted`; CheckMark/SliderGrab `kGold`; Button/Header/FrameBg = `kGold` @ .25 / .40 (hovered) / .55 (active).
- Section titles: selected display font at `kMd`. Body: default font.
- DPS meter: top-right, margin `space::k6`·Ui, panel `kPanel` @.45, radius `kMd`; DPS `kMd` `kGold` (inactive `kTextMuted`), detail line `kSm` `kTextSoft`.

### Inventory sort strip (#21)
Maintainer 2026-10-08: item sort lives INSIDE the game's inventory UI, never in the Insert window.
- Anchor: the game's bag header (`UWidgetitemBagHeaderMenu_C`, the one with the Sort button), above it right-aligned, gap `kGapToHeader` 6·Ui; below it when no room. Shown only while the header is visible and the game cursor is on. At the bank: the inventory bag's header.
- Anatomy, one row: muted "Sort by" · profile combo (`kComboW` 120·Ui) · "Weights" button → popup (SliderFloat 0–2 per weapon attack type and per stat on the player's items, "Reset profile") · muted "First" · kind combo + attack combo (`kFilterW` 96·Ui) · search field (`kSearchW` 120·Ui). A second muted line only when a sort was refused.
- Look: the Menu/HUD panel tokens above (kPanel plate, kPanelEdge border, kGold @ .25/.40/.55 frames), all text in the selected display font at `kSm`·Ui (closer to the game's display type than ImGui's default font).
- Do: leave the game's own Sort button and X key as the only sort action. Don't: add a second sort button or list items outside the game's grid.

## Shelf (prior art used)
WoW item-quality colours (rarity) · Robert Penner easings (ease) · OKLab ΔE (Ottosson) + WCAG 2.x contrast (test) ·
Material motion naming (duration + curve tokens). Deferred: ImGui ≥ 1.92 dynamic fonts (crisp text > 64 px);
game-icons.net icon font (CC BY 3.0) if primitive glyphs stop scaling.
