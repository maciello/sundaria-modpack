# Game data from the pak (`just data`)

Offline, no game running. The SDK (`sdk.py`) gives layouts; this gives values and Blueprint logic.
Reader: CUE4Parse (the library behind FModel), wrapped by `scripts/data/Program.cs`; front end `scripts/data.py`.

```bash
just data find 'RapidShot'                       # asset paths (regex, case-insensitive)
just data find 'Item' --class '^DataTable$'      # by class, from AssetRegistry.bin (/Game/... paths)
just data show GE_RapidShot_DamageInfo           # all exports as JSON (unique file name, pak path or /Game path)
just data table Weapon_Stats                     # DataTable / CurveTable rows as YAML (BP field GUID suffixes stripped)
just data bp BP_GameAbilityBase ApplyTransientDamageInfoGE   # Kismet bytecode as statements; function = regex, omit for all
just data grep 'Ability_RapidFire_Damage' --in 'Abilities/Ranger/'   # search values; --in exports matching assets first (cap 5000)
```

```yaml
first_run: just data-setup (.NET 10 SDK into ../tools/dotnet unless one is on PATH); the reader builds itself on first use
local_only:   # all next to the main clone, outside the repo; never commit any of it
  ../tools/sundaria-aes.key:  pak AES key, found automatically on first use (below)
  ../tools/data-bin/:         built reader + Oodle lib (CUE4Parse downloads it)
  ../data-cache/:             paths.txt, registry.txt, export/<pak path>.json; wiped when the pak's size/mtime changes (patch)
key: "reader `key` scans the shipping exe for 8 consecutive `mov dword [mem], imm32` (how UE embeds it) and keeps the
      candidate that decrypts the pak index to its mount point `../../../`. Game patch with a new key: delete the key file."
pak: one Archon-WindowsNoEditor.pak, v11 (UE 4.27), encrypted index, Oodle + Zlib
properties: versioned (tagged) — no .usmap mappings needed
speed: paths ~1.5 s, one export ~0.1 s, 5000 exports ~30 s
bp_output: "<offset>  <statement>"; `push N` / `pop` = execution-flow stack (sequence nodes), `goto` targets are offsets.
  Unknown tokens print as Name(field=value, …) — nothing is dropped. Locals named CallFunc_<Fn>_ReturnValue are call results.
```

Gotchas
- GameplayEffect BPs keep their numbers in the class default object (`show`, export `Default__*`, `Properties.Modifiers`), not in functions.
- Child BP defaults list only overridden properties; follow `SuperStruct` to the parent for the rest.
- User-defined enums show as `NewEnumeratorN`; the value is the index in the enum asset's `Names` (`just data show <EnumName>`).

Worked example: the full damage formula was read this way (`ApplyTransientDamageInfoGE`, `BP_GameplayCombatLibrary`,
`CustomCalculation_Damage`); the derivation lives outside the repo in `../dps-sim/data/pak/damage-formula.md`.
