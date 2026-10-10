# Design system

This file is the language and the component specs. Rule: `.claude/rules/design.md`. Missing a spec → label `design` on the issue.

## Two surfaces
| surface | what | build with | look |
|---|---|---|---|
| game screens | anything inside inventory, bank, character, merchant, options …⊇ | the game's own UMG widgets and styles, added on the game thread: `references/game-ui.md` | the game's: Narkisim text, Button05/Button04 art, stone frames, accent orange, yellow highlights. No `style.hpp` colours |
| our overlays | HUD drawn over the world: damage numbers, health bars, cast pips, loot markers, boss card, DPS meter; the Insert menu (feature flags only) | ImGui draw lists, tokens in `mods/dos-tool/core/style.hpp` (asserted by `core/test/style_test.cpp`) | Genshin-inspired language below; chrome (`kAccent`, `kPanel`) pulled towards the game's palette so overlays next to game UI match |

Start from the game's base UI (`game-ui.md`); reference images from other games (`../design-refs/<topic>/index.yaml`, local only) come after, as inspiration.

Overlay renderer: Dear ImGui 1.90.9 draw lists on the D3D11 overlay. No textures. Text = embedded display fonts
(`mods/dos-tool/assets`, Titan One default) baked at `type::kAtlasPx` 64 px: bigger text is upscaled and soft.
Colour to ImGui: `style::Pack(token, alpha)` == `IM_COL32`.

## Language
Genshin-inspired: clean outlined numerals, soft same-hue glows, crisp silhouettes, restrained motion.
Borderlands only for the boss name card (skewed slab, hard shadow, bold title).

| principle | rule |
|---|---|
| readable on anything | every glyph/text = fill + `color::kInk` outline at `stroke::kOutlineAlpha`; fills ≥ 4.5:1 on ink (tested) |
| one hue, one meaning | element colours only for elements, rarity only for loot, `kHpFill`/`kTaken` only for HP loss, `kAccent` only for chrome (trim, menu, HUD accent). Never colour by size |
| size = magnitude | log scale, clamped (`type::kNumberMin..kNumberMax`), stacks capped at `kStackCap` |
| restrained motion | one primary motion per element; 0.12–0.6 s; overshoot (OutBack) only at birth; no looping motion except idle loot cues |
| soft glow | glow = same hue as the fill, wide, low alpha (`stroke::kGlowWidth`, `kGlowAlpha`); white only as an impact flash ≤ `motion::kFlash` |
| crisp silhouette | shapes from filled primitives; min stroke 1 px; min icon radius `type::kIconMinR` |
| scale | HUD: × `type::Ui(h)` (1080p = 1). World-anchored: × Ui × depth, depth = clamp(1500 cm / distance, 0.6, 1.25) |
| colour redundancy | colour is never the only channel: element = colour + icon, rarity = colour + beam/glow strength, suggestion = colour + glyph + text |

## Tokens (`style::`)
```yaml
color:   {kInk: ink outline 20,12,8, kShadow: drop shadow, kText: 255 white, kTextSoft: 235,225,205, kTextMuted, kAccent: chrome accent = game accent orange 255,166,69,
          kPanel: 18,15,22 @.84, kPanelEdge: accent @.35, kGameText: 239 game button text, kGameHighlight: 252,255,0 game counts/marks, kGamePositive: 21,200,0 game stat-compare better, kTrack, kHpFill, kHpSheen, kHpChip, kHeal, kTaken, kGood: upgrade, kSell: sell}
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
          kPipFill .18 OutBack 2, kPulsePeriod 1.6, kCardIn .45, kCardOut .35, kCardHold 2.6}   # seconds
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
- Anchor (HUD): row centred at (w/2, `hud::kPipsY`·h), above the character's feet in the default camera.
- Pip: diamond, half-diagonal 6·Ui, edge gap `space::k2`·Ui. Three states, each one step brighter:
  - empty (not shot yet): `kInk` @.55 + 1 px outline `kTextSoft` @.6.
  - **fired** (its hit notify passed: arrow out / swing done, no hit confirmed): hit colour @`kFiredAlpha` .35 fill + 1.5 px outline in the hit colour over a 3 px `kInk` outline. No glow, no flash. A miss stays fired: every shot always fills something.
  - **hit** (a record by the hero landed): ability element colour (Physical `kText`) solid + glow disc radius 1.8× at `kGlowAlpha`.
- Size: every pip the same. A bigger pip needs a deterministic per-hit difference (e.g. an empowered last shot) read from game data; landed damage is not one (crits, same-frame sums, multi-target), so learned weights were removed (#85).
- N > 10: one segmented bar (w = `hud::kBarW`·Ui, h = `hud::kBarH`·Ui, `Pill`) with N−1 ink ticks; segments fired (hit colour @.35) / hit (solid) like pips.
- Spread abilities (cone/volley, today Salvo): no pips; their notify count is not hits per target (#81). Volley pips once the notify dump shows how ApplyEffectID groups the notifies.
- Motion: row fades in `kFadeIn`; fired: pip scales 0.4 → 1 with `kPipFill`; hit: the same pop again + white flash `kFlash` (upgrade reads as a second beat); all hit: row punch 1.15 → 1 over 0.25 s OutCubic, hold 0.5 s, fade `kFadeOut`. Cast ended: pips never shot turn `kTextMuted`, row fades after the late-hit window (`kLateHits`). No red, no shake.

### Wind-up ring (cast indicator, #92) — the one "when does it fire" cue
- When: the next hit of the cast is ≥ `kWindupMin` (0.3 s) away at its start, at the live play rate (attack speed). Covers plain wind-ups (Paralysing Shot, AimedShot) and hold abilities (PoisonArrow, Hemlock, DeadlyAim …⊇: ShootArrow with CanHold), where it closes over the draw (Pull section) and meets at "armed". Shorter wind-ups: no ring. No other bar shows this moment (charge bar retired).
- Anchor (HUD): centred on the first pip (row centre for N > 10 or no pips). A single-hit cast with a wind-up shows one pip inside the ring.
- Target ring: r `kRingR` 11·Ui (clears the pip glow), line `kBarEdge`/2 ·Ui `kTextSoft` over a `kInk` underlay (+2 px) at `kOutlineAlpha`.
- Approach ring: r `kApproachR` 44·Ui → `kRingR`, **linear in time** (constant closing speed reads as a timing, not progress), meets the target ring when the shot can first fire. Line `kBarEdge`·Ui `kTextSoft` + ink underlay + same-hue glow (`kGlowWidth`, `kGlowAlpha`).
- Armed (met, not fired: holding, or the notify is a tick away): one ring at `kRingR`, line `kBarEdge`·Ui `kText` + ink underlay + `kText` glow. Static: no pulse while held.
- Fired (the hit notify passed / the arrow spawned): target ring punches 1 → `kReleasePunch` 1.35 OutCubic over `kRelease` 0.25 s, white flash `kFlash`, alpha 1 → 0 over the same 0.25 s. The pip turns fired at the same moment.
- Cancelled (ability ended before firing, e.g. dodge, also while held): both rings `kTextMuted`, fade `kFadeOut`. Fade in `kFadeIn`.
- Don't: a second bar for the same moment, pulsing, red, shake.

### Loot marker: rarity glow, beam, idle shimmer (#25, #27)
- Anchor: projected item position. depth as above; cull beyond 30 m, fade 25–30 m.
- Tier < `kGlowFrom`: ground ring only, alpha .5. Tier ≥ `kGlowFrom`: ring + glow. Tier ≥ 4: + beam. Tier ≥ 5: + drop burst.
- Ring: Ellipse radii (18, 6)·Ui·depth, stroke 2·Ui in tier colour @.8, filled @.18.
- Glow: 3 concentric CircleFilled r = (10, 16, 24)·Ui·depth, alpha .30/.15/.07, tier colour.
- Beam: RectFilledMultiColor, width 6·Ui·depth, height 90·Ui·depth up from the ring centre, bottom tier @.55 → top tier @0.
- Idle sparkle (unlooted only, #27 / #110: `features/loot/idle-loot`, constants in `idle-loot.hpp`). Ref: WoW lootable-corpse sparkles. Maintainer 2026-10-09: "too obvious… more particles and less often and more random… show the best colors"; then "visible through walls AND ui/gui", "go in the wrong direction when moving the camera", "more like burst not a random a little more continuous sparkle", "can we use unreal engines particle system".
  - Medium: the game's Cascade system `hp_mag_alchemyOrb_fireflies` (`kTemplate`) attached to the loot actor's root through `core/fx.hpp` (catalogue `fx.md`): in the world, depth-tested, under the game UI. Never ImGui (`.claude/rules/design.md`).
  - Motion (the template's, not ours): 5 motes orbit the anchor for ever, ≤ 20 cm out, 0.1–0.25 turns/s on random axes; each flickers at its own random rate (material dynamic parameter 0.05–5). Continuous, sparse, unsynchronised; no bursts.
  - Anchor: root + 15 cm (items, `kLiftItem`) / + 45 cm (chests, `kLiftChest`); component scale 1 (items) / 2 (chests); cull 30 m (`kCull`).
  - Colour: material instance per component, vector `Emissive Thorax` = the game's `GetItemColorForGrade` (sRGB → linear) × `kGlow` 3 of the best grade among unlooted loot within 150 cm (`kPileR`); no known grade (closed chest) → `kTextSoft`, never a guessed tint.
  - Looted / gone: the component is destroyed on the next world tick after the game's loot event (`OnTriggerChanged`, `OnLootReady`, `OnRep_LootIsReady`, `MulticastPlayPickupSound`, `I_SetInactiveLoot`, `ReceiveEndPlay`); state re-read for 2 s (`kSettle`). Destroyed actor: its component goes with it.
  - Tuning knobs: `kGlow`, `kScaleItem`/`kScaleChest`, `kLift*`, `kCull`; rate/count/orbit belong to the template (no instance parameters).
  - Don't: overlay shapes, glow discs, rings or beams at rest; a fixed period; scale pulses.
- Drop: ring + glow scale in with `kPop`; beam height 0 → full over 0.45 s OutCubic; burst = 8 radial lines, length 0 → 28·Ui, alpha 1 → 0 over 0.5 s, tier colour.
- Looted / emptied: everything fades `kFadeOut` (≤ 1 s).
- Don't: pulse scale (only alpha), draw through walls without fade, use element colours.

### Chest open burst (#24, overlay option A)
- On closed → open: flash disc r 40·Ui·depth `kTextSoft` @.5 → 0 over 0.25 s; 12 sparks = radial lines from r 0 → 60·Ui·depth (OutCubic, 0.6 s), length 10·Ui → 0; 6 motes (CircleFilled 2·Ui) rising 80·Ui over 1.2 s with drift ±10·Ui, alpha → 0.
- Colour: highest item tier in the chest if known, else `kTextSoft`. Particles deterministic from hash(actor id) (SDK-free, tested).

### Pickup toast (#64, game widgets on the HUD)
- Ref: `genshin-obtained-feed` (rows of icon + rarity-coloured name, right of the character, slide in).
- Row: the game's own loot-toast row `WidgetLootToastEntry_C` (40×40 atlas icon, `ItemIcon_QualityBorder`, name Narkisim 24 with black
  outline), filled by the game's `OnListItemObjectSet` from {name, icon id, grade}; name and quality border tinted with the game's
  `GetItemColorForGrade`. Icon and rarity colours are the game's, none ours.
- Place: viewport anchor (0.64w, 0.56h), row's bottom-left on the anchor; newest at the anchor, older rows move up 48 units
  (OutCubic 0.18 s). At most 5 rows; a 6th fades the oldest out at once.
- Motion: slide in from +56 units right, OutCubic 0.28 s, opacity with it; hold 4 s; fade `kFadeOut` (0.4 s InQuad). No bounce, no loop.
- Constants: `features/inventory/pickup-toast/pickup-toast.hpp`.
- Don't: toast reorders, sorts, equips or bank moves (only items whose owned count rose); ImGui rows; own rarity palette.

### Dungeon map path (#40, inside the game's minimap)
- Where: plain `UImage`s (no texture = solid tint) in `WidgetMiniMap_C::CanvasPanel_DynamicMinimp`, placed like the game's own icons
  (anchors (0,1), alignment 0.5, map pixel = (Y, −X) / UnitToPixel). They pan, rotate and clip with the minimap; nothing on the HUD outside it.
- Main route: the floor's main line, entry → stairs down (exit volume on the last floor), drawn from the furthest point the player
  has reached (within 6 m of it; the walked part disappears, never reappears on walking back) to the goal, one bar per path
  segment, 2 px thick, `kAccent` @.55. Goal cap: 7 px diamond, `kGameHighlight` @.9, at the end. Replans only on door, lever and floor events.
- Connector (#83): only while no point of the main route lies inside the minimap's view (inscribed circle of the retainer, from its
  geometry × UnitToPixel; hides again once the route is within 90 % of that radius). Dashed: 6 px dashes, 5 px gaps (at most 64, spread
  evenly), 2 px, `kAccent` @.8, no flow; 5 px `kAccent` diamond @.8 where it joins. From the player (1 m steps) to the nearest point of the
  main route at or after the furthest point they have reached (never back past it). Replans on a room change or 8 m off it, ≤ 1/s.
- Flow: 5 px diamonds `kGameHighlight` travelling entry → goal at 40 px/s, 36 px apart (at most 24; spacing grows on long lines);
  each fades in over the first 16 px and out over the last 16 px before the goal. Linear, no easing: it reads as current, not as bounce.
- Blocked: where the path stops at a locked door: 16 px `kInk` diamond plate @.85 + red X (two 11×3 bars, `kTaken`).
- Lever (heuristic: unpulled levers in the locked door's room): 14 px `kInk` plate @.85 + `kGameHighlight` handle bar 9×3 at −60°.
- Levers that open the way (#93): the route runs player → nearest unpulled lever → next → the locked door → on (same line and flow).
  Minimap: a 22 px `kGameHighlight` halo diamond behind the lever plate (z of the flow), alpha 0 → .35 → 0 over `motion::kPulsePeriod` (sine).
  World (ImGui background list, Layer::WorldBar, within 60 m, hidden at 2 m): 12·Ui `kGameHighlight` diamond with a `kInk` rim
  (`stroke::Outline(kSm·Ui)`) 1.2 m above the lever, distance `kSm` `kTextSoft` ("12 m", ink outline) below it. Off screen: the diamond on the
  edge (inset `space::k7`·Ui) + a chevron of two 9×3·Ui `kGameHighlight` bars pointing at the lever. In/out: `motion::kFadeIn` / `kFadeOut`.
  Toggle: "Lever markers" owns every lever visual (minimap lever icon + halo, world marker); dungeon-map draws the minimap layer
  only while it is on (shared planner state). Constants: `features/dungeon/lever-markers/` (world marker), `dungeon/dungeon-map/scene.hpp` (halo).
- Icons stay upright on screen (counter-rotate with the map). The game's own icons (party, NPCs) stay above the line (z: line 0, icons 5).
- Constants: `features/dungeon/shared/path.hpp` (path, flow), `dungeon/dungeon-map/scene.hpp` (geometry, opacity, z).
- Don't: own fog; textures we ship; ImGui over the minimap.
- Superseded: "line only up to the frontier" (#40, 2026-10-08). Maintainer, 2026-10-09: "and for the map indicator i meant the MAIN ROUTE
  indicated and if the player is TOO FAR AWAY to see it on the map, then a route TO the main route".

### Suggested sell/salvage (#23, game screen)
- Where: every item slot of the game's bags (`WidgetItemIconContainer_C` in `WidgetItemBag_C::ItemContainers`): inventory, bank, vendor.
- Mark: the game's own action icon (`Tooltip_Sell` / `Tooltip_Salvage`, the icon its item tooltip shows) as a UImage 32×32 in the slot's
  `Overlay_Container`, top-left, padding 6. Hit-test invisible (clicks and drags reach the slot). Top-right and bottom-right stay the game's
  (select mark, comparison icon, stack count).
- Reason: one line in the game's item details panel (`WidgetItemDisplayDetail_C`), appended under its text, styled like the panel's own
  "Learned" line (Narkisim 16, orange 1, .651, .27, centred, wraps): `Sell suggested: worse than <item>` / `Salvage suggested: …`.
- Code: `features/inventory/item-sell/inventory-badges.cpp` on `inventory/shared/tiles.hpp` + `marks.hpp`; data: `item_sell::api::Suggested()`.
- Don't: own colours or ImGui over the bag; selecting items for the player (the game's Select mode stays theirs).
- Upgrade marks: § Item upgrade marks (bottom-left; this mark keeps the top-left).

### Item upgrade marks (#117, game screen)
- Where: the same item slots as the sell marks (`WidgetItemIconContainer_C` in `WidgetItemBag_C::ItemContainers`, inventory and bank),
  equipable items only. Corner: bottom-left (top-left = sell mark; right side = the game's select mark, comparison icon, stack count;
  equipables never show a stack count).
- Anatomy: one chip per slot = the game's own count chip (`WidgetItemIcon_C::Border_Count`: `Seperator` texture, Box margin 0.2,
  tint black @.75) holding one TextBlock, Narkisim (font copied from the slot's `TextBlock_Count`), size `kChipFont` 20, shadow (1,1) black
  like the count. Chip padding 6,2,6,0 (`kChipPad`), slot padding 4 from the left and bottom edges (`kChipInset`). Hit-test invisible.
- States, one at a time, first match wins:
  | state | when | text | colour |
  |---|---|---|---|
  | upgrade | equipping it in its best slot raises the current hero's DPS by ≥ `kMinGain` 1 % | `+4.2%` (one decimal; ≥ 10 %: `+12%`) | game `ComparisonColorPositive` = token `kGamePositive` |
  | other hero | no upgrade here, ≥ `kMinGain` for another saved hero (`dos-tool-chars/<slot>.yaml`) | that hero's name, ≤ 8 chars (cut, no ellipsis) | `kAccent` (the game's accent orange) |
  | downgrade | equipping it lowers the current hero's DPS by ≥ `kMinGain` | `-4.2%` | game `ComparisonColorNegative` = token `kGameNegative` |
  | none | otherwise (\|change\| < 1 %, doesn't fit), or no DPS data yet | chip collapsed | — |
- Detail lines (the game's item details panel, styled like its orange "Learned" line, as the sell reason): `Upgrade: +4.2% DPS` / `Downgrade: -4.2% DPS`,
  `Better for <hero> (<class>): +6.1% DPS` (best other hero), `Best in slot: <class>[, <class>]` (no owned item of that class's slot beats it).
- Colour is never the only channel: the "+" / "-" and "%" say upgrade / downgrade, a name says other hero.
- Motion: none (the game's tiles have none).
- Code: `features/inventory/item-upgrade/` (constants + verdict logic `item-upgrade.hpp`, scores `scores.cpp`); chip widget `inventory/shared/marks.hpp` `ChipIn`. Dev install without DPS tables: preview marks (every third equipable an upgrade, every third "Preview"), details say "Preview, no DPS data yet".
- Don't: ImGui over the bag, own art, a second chip per slot, changes below `kMinGain` either way (noise), marks on consumables.

### Boss name card (#19, Borderlands style)
- Cinematic layer: hides WorldNumber/Hud while shown. Letterbox: black rects top and bottom, 0 → 0.1h over 0.4 s InOutCubic, out the same.
- Plate: parallelogram skew 12°, anchor left edge at 0.08w, vertical centre 0.62h, height 96·Ui, width text + 2·`space::k8`·Ui, fill `kPanel`; `kAccent` trim lines 3·Ui along top and bottom edges.
- Subtitle (epithet / FightStartedMessage) `kMd` `kTextSoft` above the name; name `kXl` `kText`, ink outline + hard drop shadow (offset (4, 4)·Ui, `kInk` @1).
- Motion: plate slides in from −40·Ui with `kCardIn`; name scales 1.3 → 1 over 0.2 s OutCubic after 0.1 s; hold `kCardHold`; out `kCardOut` slide +40·Ui + fade. Skip → all out in 0.1 s.
- Fallback: no name → class name with `BP_`/`_C` stripped. Names > 18 chars → `kLg`.

### Boss intro camera (#18)
- Sequence ≈ 4.6 s, skippable: approach 1.2 s InOutCubic → hold `kCardHold` with a linear 4°/s orbit → return 0.8 s InOutCubic to the live gameplay pose (re-read every frame).
- Framing: boss on the right third, eye level −10°, distance ≈ 2.5 × boss capsule height, FOV −10° vs gameplay. Letterbox + hidden HUD for the whole sequence. Always the face: the camera stands within `cam::kFaceArc` (60°) of the boss's forward, on the player's side when that is in front, and the orbit turns towards the front (#99). A blocked front: the clearest angle of that arc, pulled in like a spring arm (#71); never the back.
- Don't: cuts, zoom punches, shake, roll. Borderlands energy stays in the name card.

### Menu (Insert window) and HUD panels
- Insert window = feature flags only (toggle + that feature's tuning). No feature UI (lists, panels, item tools): those go into game screens.
- ImGuiStyle: WindowRounding `kLg`, FrameRounding/GrabRounding `kMd`, WindowPadding (k6, k6), FramePadding (k4, k2), ItemSpacing (k4, k3); WindowBg `kPanel`; Border `kPanelEdge`; Text `kText`; TextDisabled `kTextMuted`; CheckMark/SliderGrab `kAccent`; Button/Header/FrameBg = `kAccent` @ .25 / .40 (hovered) / .55 (active).
- Section titles: selected display font at `kMd`. Body: default font.
- DPS meter: top-right, margin `space::k6`·Ui, panel `kPanel` @.45, radius `kMd`; DPS `kMd` `kAccent` (inactive `kTextMuted`), detail line `kSm` `kTextSoft`.

### Inventory sort profile (#21, game screen)
- Where: bag header row (`WidgetitemBagHeaderMenu_C`, inventory and bank), directly right of the game's Sort.
- Widget: the game's `WidgetButton01_C`, Button style copied from Sort (Button05 art), label colour = Sort's (0.937), Narkisim 18, slot = Sort's (fill 0.5). Same size and look as Sort; the row reads `[Sort] [Profile: Melee]`.
- Label: `Profile: <name>` (Balanced, Melee, Ranged, Magic …⊇ from `item_sort::api::ProfileNames`).
- Click: next profile (wraps) and re-sorts that bag at once (`RequestSort(IsStorage)`). The game's Sort also follows the active profile.
- States: hover/pressed = the game's Button05 art. Item sort off: the button stays inert until the game rebuilds the screen.
- Weights stay in the Insert menu (tuning of the flag).
- Don't: ImGui strips/windows over the bag, own colours, reuse `Button_SpecialOption0` (it is Destroy Items).

## Shelf (prior art used)
WoW item-quality colours (rarity) · Robert Penner easings (ease) · OKLab ΔE (Ottosson) + WCAG 2.x contrast (test) ·
Material motion naming (duration + curve tokens). Deferred: ImGui ≥ 1.92 dynamic fonts (crisp text > 64 px);
game-icons.net icon font (CC BY 3.0) if primitive glyphs stop scaling.
