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

## Blueprint cross-reference index (`callers` … `ast`)

"How does the game do X" in one command, over every Blueprint (9143 packages incl. level scripts, widgets, anim BPs).
```bash
just data callers OnProjectileHit                 # definitions (+flags), call sites by kind, delegate bindings
just data writers LockStatus                      # assignments (Let, struct member, Array_/Map_/Set_ mutators' first arg)
just data readers LockRules_OR
just data calls BP_ProjectileBase::ReceiveHit --depth 3   # outgoing tree; an event follows only its ubergraph part
just data events BP_Door                          # event entry points (ubergraph offset) + delegate bindings
just data ast BP_TriggerBase::CanUnlockByRule     # bytecode as JSON AST (object refs 'Owner:Name', props {Name, Type, Owner})
```
```yaml
hit: "<Class>::<Function> @<statement offset> [<events reaching it>]"   # offsets = `just data bp` offsets
events_in_brackets: ubergraph statements reachable from each event stub's entry (jumps, push/pop, latent resume); a list = shared code
call_kinds:   # UE 4.27 ScriptCore.cpp; none of the script call opcodes enters UObject::ProcessEvent
  final:   "EX_FinalFunction / EX_LocalFinalFunction / EX_CallMath: UFunction fixed at load → CallFunction / ProcessLocalFunction → native Invoke or ProcessScriptFunction"
  virtual: "EX_VirtualFunction / EX_LocalVirtualFunction: FindFunctionChecked(name) at run time (a subclass override wins), then the same path. 'Local' = script callee, not 'self'"
  bcast:   "EX_CallMulticastDelegate: ProcessMulticastDelegate → ProcessDelegate → ProcessEvent on every bound function"
process_event_hook_sees: delegate-bound functions (bound/bindings), FUNC_Event overrides called by native code (ReceiveHit, ReceiveBeginPlay, K2_*), received RPCs (unverified here), timers by delegate
process_event_hook_misses: every Blueprint→Blueprint call above, incl. interface calls and overrides of native BlueprintEvents when a Blueprint calls them
calls_tags: "[native] = engine/C++ leaf; [native class; …] = by-name call on a C++ type (a BP subclass may override); [unresolved] = BP class chain has no such function (interfaces, by-name on unknown type)"
not_indexed: locals and temporaries (only instance/default vars and struct members); C++ bodies; default-object values (use `show`)
storage: ../data-cache/script/ (logic-only exports: functions, classes, delegate bindings) + xref.sqlite; built on first xref command, then only new packages; pak stamp change wipes both
timings: cold 129 s (exports 9143 packages + index); index from warm script/ 4 s; queries 0.25-0.35 s
```

Gotchas
- GameplayEffect BPs keep their numbers in the class default object (`show`, export `Default__*`, `Properties.Modifiers`), not in functions.
- Child BP defaults list only overridden properties; follow `SuperStruct` to the parent for the rest.
- User-defined enums show as `NewEnumeratorN`; the value is the index in the enum asset's `Names` (`just data show <EnumName>`).

Worked example: the full damage formula was read this way (`ApplyTransientDamageInfoGE`, `BP_GameplayCombatLibrary`,
`CustomCalculation_Damage`); the derivation lives outside the repo in `../dps-sim/data/pak/damage-formula.md`.
