# World effects: game particle systems (`core/fx.hpp`)

A sparkle, glow or marker on something in the world = pick a game template below, then set a few values.
Don't draw it with ImGui (`.claude/rules/design.md`), and don't search the pak again: `just data fx` already did that.

```cpp
#include "fx.hpp"   // game thread: inside a game::On / game::OnWorldTick callback
fx::Place p; p.z = 15; p.scale = 1.0f; p.cull = 3000;                          // cm offset from the actor root, size, cull
fx::Id id = fx::Attach("my-feature", L"/Game/FX/Environmental/Particles/fx_fireFlies.fx_fireFlies", actor, p);
fx::MaterialColor(id, 0, L"Emissive Thorax", 2.0f, 0.1f, 2.5f);                 // emitter 0's material vector param, linear, HDR ok
if (looted) fx::Remove(id);                                                     // or let it die with the actor
void Off() override { fx::Release("my-feature"); }                              // every effect of the feature, via the world tick
```

```yaml
api:   # core/fx.hpp; all game thread except Release
  Attach(owner, path, actorOrSceneComponent, Place) -> Id   # 0 = template missing / no parent; actor = its RootComponent
  At(owner, path, Place) -> Id                              # world location
  Color|Float|Vector(id, name, …):  instance parameters (Cascade *ParticleParameter name; Niagara "User.Name")
  MaterialColor|MaterialFloat(id, element, name, …): dynamic material instance on emitter `element` (created once)
  Alive(id), Remove(id), Release(owner)
knobs: {Place.x/y/z: cm offset (attached) or world cm, Place.scale: uniform (volume + sprite size + speed), Place.cull: cm, colour: linear RGB × intensity}
not_knobs: spawn rate, count, lifetime, orbit: baked into the template (Cascade has no instance params for them); pick another row
cost: each call one ProcessEvent; templates loaded once per path; core subscribes the world tick only while effects exist
colour_routes:
  instance_param: none of the game's Cascade systems has one; Niagara User.Color on NS_Skill_Portal*, NS_Ability_Avatar/Sacrifice/ShadowCloakA, Blood_VFX P_* …⊇
  material_param: "MaterialColor with the colour_params name `just data fx` prints per emitter (fx_fireFlies: Emissive Thorax)"
  particle_colour_only: M_FlickeringSparkle_*, M_ky_starDust, fx_base_flareBASE, M_GFXLU/GPS DustGlow …⊇ (fixed colour, or swap the template)
```

## Catalogue
`just data fx <path regex>` → usable templates (Cascade sprites, ≤ 3 emitters, spawn volume ≤ 150 cm, sprites ≤ 40 cm, no
light, continuous or never-dying): 82 of 808 systems (664 Cascade + 144 Niagara) on 2026-10-09. `--all` lists all 808.
Per emitter it prints: rate or burst, lifetime (`forever` = never dies), volume_cm, size_cm, material, colour_params.
Look: read from those numbers, not seen in game unless a shot is named.

| template (`/Game/…`) | em. | mode, life | colour route | look |
|---|---|---|---|---|
| Environments/HumanProps/Magic/Particles/hp_mag_alchemyOrb_fireflies | 1 | burst 5, forever | material `Emissive Thorax` | 5 motes orbiting ≤ 20 cm, each flickering at its own rate; idle-loot uses it (shot: pending) |
| FX/Environmental/Particles/fx_fireFlies | 1 | burst 5, forever | material `Emissive Thorax` | same fireflies, orbit 50-100 cm; its light-module emitter is off at LOD 0 |
| FX/Classes/Champion/Particles/P_Champion_HandGlow_01 | 2 | 200/s 0.15-0.3 s + light rays | none | dense flickering sparkle ball ≤ 5 cm (a hand glow) |
| FX/Classes/Wizard/Particles/P_StaffGlow_01 | 2 | 50/s 0.15-0.7 s + ray burst | none | orange sparkle spray ≤ 10 cm (staff tip) |
| FX/Magic/Particles/fx_HolyLightTrail | 1 | 5/s 0.5-1 s | material `Emissive Color` | soft glow puffs 20-30 cm |
| FX/Enemies/FX_Fire_Skeleton__Embers | 2 | 2/s 0.75-2 s embers + flame | none | small flame with rising embers |
| FX/Environmental/Particles/P_GodRays_01 | 1 | 5/s 3-8 s | none | light shafts |
| FX/Enemies/P_Eye_{fire,green,ice,purple,red,yellow}_01 | 2 | 12/s + 10/s | none (one template per colour) | glowing eye wisp ≤ 1 cm volume |
| Marketplace/GoodParticleStatus/…/PS_GPS_SimpleSmoke_{Aqua,Blue,DarkGreen,Green,Orange,Purple,Red,White} | 1 | 5/s 1-4 s | none (one per colour) | coloured smoke puffs 25-33 cm |
| Marketplace/AdvancedMagicFX12/particles/P_ky_aura_yellow | 3 | burst 1 each, 1 s | material `dustColor`, `hilightColor`, `lowLightColor` | short yellow aura flash |
| Blueprints/…/PurpleLantern/PS_PurpleLantern_ImmunityBubbles | 1 | 20/s 1 s | none | bubbles rising in a 50 cm sphere |
| Blueprints/Ability/Abilities/Generic/UsePotion/fx_potion | 1 | 10/s 1 s | none | 1 cm sprites, 10/s at one point (look unknown) |

Rejected for small world markers (`--all` rows), reason:
- fx_env_flowerPollen_Glow: 450 cm cylinder, life 25-50 s, warmup 50 s → room-wide haze
- fx_BossChest_Glow: 4 emitters incl. a dynamic light and 250 cm flares → big chest glow
- P_ky_hit_shine, P_ky_trail_shine, P_ky_trail_shineDust: bursts on hit / per unit moved, loops 1
- P_Elemental_ice_ambient_01: smoke + 30-50/s sparkles in 20-50 cm
- dun_cas_lootPile_activateA: coin mesh burst on collect
- fx_weapon_*Mist / holyLvl2: weapon auras, 3-5 emitters incl. smoke …⊇

Add a row here when you have seen a template in game (`just game shot` path) or used one in a feature.
