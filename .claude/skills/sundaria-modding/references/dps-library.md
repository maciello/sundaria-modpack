# DPS library (`libs/dps`, #118)

One SDK-free C++ model of hero DPS. The same code runs in `DoS-Tool.dll` (item marks, #117) and offline (`just dps`, no game).
Game data never enters the repo: the tables come from a Source at runtime.

## API (`libs/dps/include/dps/dps.hpp`, stable)
```yaml
Model::Load(Source&, err):   once; nullptr + err when tables are missing
Prepare(build, scenario):    compile a hero once (both weapon sets: 2H set, 1H set); immutable, any thread
Dps(build|prepared):         {dps, weaponType, abilities [dps, damagePerCast, castSeconds, cooldown, casts], ignoredStats, error}
ScoreItem(prepared, item, slot=-1): {deltaPct, dps, replacesSlot (-1 = empty slot), fits}; best fitting slot; worn item = 0
BestInSlot(hero|class+level, scenario, candidates): coordinate ascent per weapon type -> picks {equipSlot, candidate index}
StatWeights(build, scenario, grade): % DPS per affix as it rolls on each slot the class wears, best first
inputs: Item {spec, equipSlot, weaponType (spec name), grade, level, slot (equipped only), stats {EStatType name: value}}
        Build {cls, level, primary[6], abilities (snapshot names -> level), abilityLevel, heroism, equipped}
        Scenario {targets, fightSeconds, targetLevelDelta, armor, magicResist, glancing, deflect, incomingDamageMod, resist, eventSim}
```

## Files (one responsibility each)
| file | does |
|---|---|
| `include/dps/tables.hpp` | `Tables`: every game table the model reads, plain data (what a Source fills) |
| `include/dps/source.hpp`, `src/tables_io.cpp` | Source interface; `FileSource` = text tables file (format at the top of tables_io.cpp) |
| `src/items.*` | crafting-bonus item generation: value, pick lists, spawn chance, class armor type / tags |
| `src/compile.*` | class x weapon x ability levels x scenario -> kernel arrays (montage pick, cast, hits, stat -> attribute map) |
| `src/formula.hpp`, `rotation.hpp`, `kernel.*`, `layout.hpp` | one hit (game-facts `dps_mechanics`), fluid / event rotation, the per-row kernel |
| `src/model.cpp`, `prepared.hpp`, `score.cpp`, `bis.cpp`, `weights.cpp` | API |
| `host/c_api.cpp`, `include/dps/c_api.h` | offline only (threads; not in the DLL): `dps_eval` raw kernel, `dps_call` line requests -> YAML |
| `scripts/dps.py`, `scripts/dps_tables.py` | offline CLI (ctypes); pak extract `model.json` -> tables file |

## Data sources
```yaml
offline: tables file `build/dps/tables.txt` of the checkout (DPS_TABLES overrides; never the game dir); `just dps-tables <model.json>` writes it from the
  pak extract (local dps-sim `just extract`). `just dps-install` copies it to <Win64>/dos-tool-dps/tables.txt, which the DLL reads (FileSource); install only together with a DLL built from the same commit (a newer table format makes the running DLL fall back to preview marks).
in_game: not built (#119). Plan: a Source in features/inventory/shared reading the DataTables/CurveTables by path
  (UKismetSystemLibrary::MakeSoftObjectPath + load, as core fx.cpp does; no GObjects walk) with core reflect.
  Which tables stay loaded: UNVERIFIED (game was not running). Ability data (CDOs, montages: only equipped weapons'
  montages are loaded) is the hard part.
```

## Run offline (no game)
```bash
just dps-tables <model.json>        # once per game patch
just dps hero <dos-tool-chars/N.yaml>   # DPS + per-ability breakdown
just dps score [--items F ..]       # every item (default: all heroes' equipped) vs each saved hero
just dps bis Ranger --level 10      # best item per slot from the items
just dps weights <snapshot|Class> [--grade 3]
just dps calibrate <snapshot>       # predict each item stat from (spec, grade, level); expect all exact
just dps bench                      # kernel 10k rows + scoring 300 items
```
`--scenario boss|armoured|resist|aoe5|burst15` (synthetic), `--scenarios FILE` for local ones.

## Change the model
- New stat with a reader: map it in `compile.cpp` (`statAttr`), give it an attribute in `layout.hpp` if new, use it in `kernel.cpp`/`formula.hpp`; test in `test/dps_test.cpp` with made-up numbers.
- New ability rule (DoT row, AoE source, cooldown row): resolve it in `scripts/dps_tables.py`, carry it in `tables.hpp` + `tables_io.cpp` (format line), consume in `compile.cpp`.
- New table: `tables.hpp` field + `tables_io.cpp` record + `dps_tables.py` writer, in one commit.
- Use rules (`detail::CanUse`, game-facts `item_use_rules`): item level > hero level, or a Crossbow/Bow2H/Crossbow2H weapon for a class without ShootArrow → `fits=false`, BestInSlot skips. `just dps-tables` writes into the game dir by default: pass an `out` path (and `DPS_TABLES`) to keep a running game's tables untouched.
- Assumptions (model, unverified): main hand = WeaponAny in the lowest equip slot; the off-hand weapon adds its stats except WeaponDamage / WeaponDamage_<element>; 2 Ring + 2 Trinket slots; BestInSlot is a local optimum.
