# Ability mods (Lua + ECS)

Epic #41. Feature folder `mods/dos-tool/features/ability-mods/`. Players' scripts: `<Win64>/dos-mods/abilities/*.lua`
(examples: `mods/dos-tool/abilities/_examples.lua`, installed by `just dev-install` / `just dev` / `just dist`).

## Shape
| piece | file | tested |
|---|---|---|
| ECS registry (index+generation entities, sparse-set pools, `Each<A,B>`) | `ecs.hpp` | `test/ecs_test.cpp` |
| Lua host: sandbox, `ability.tweak/on/new`, ctx actions -> `Command`s, errors with file:line, last-good fallback | `script.hpp` | `test/script_test.cpp` |
| game systems on the game thread (core listener): cast tracking, out notify, cooldown timers, command runner | `ability-mods.cpp` | in game only |
| cooldown effects of one ability (shared with input-feel) | `core/game.hpp` `CooldownEffects` / `RemoveEffect` | in game only |

Errors (file:line), loaded declarations and `ctx:log` go to `dos-mods/abilities/log.txt` (rewritten on change): the
Insert menu holds only flags/tuning (new-ability toggles, counts), per `.claude/rules/design.md`.

Rule: scripts never touch the game. They declare (rules, hooks, new abilities) and record commands; C++ systems execute.
New action = `Command::Kind` + ctx method in `script.hpp` (+ test) + a `case` in `Run()`.

## Lua build
- Lua 5.4.9 vendored in `third_party/lua/src` (unmodified, MIT). `lua_all.cpp` = one **C++** unit (errors unwind as
  exceptions, so `luaL_error` through C++ frames is safe). Leaves out io/os/package/debug: sandbox by construction;
  the host also removes `load`/`loadfile`/`dofile` (no bytecode, no files).
- Native test `#include`s `lua_all.cpp` **after every std header**: Lua's private headers define macros (`next`, ...)
  that break std headers included later. `lua_all.cpp` defines `LUA_CORE`/`LUA_LIB` first (else `l_likely` is missing
  when `lua.h` was already included).
- Limits: instruction budget per run/handler via a count hook (endless loop -> error), memory cap via the allocator.

## Game paths used (all **unverified** in game until #42/#43 say so)
- Cast = change of `LocalAnimMontageInfo` {AnimMontage, AnimatingAbility} (`game-facts.md`). activate/end events.
- Out = `UBP_GameplayAnimNotify_C::Received_Notify` with ApplyEffect/ShootProjectile on the hero mesh.
- Anim rate: `USkeletalMeshComponent::GetAnimInstance()->Montage_SetPlayRate(montage, r)` right after the cast is seen.
- Cooldown scale: watch the cast 3 s; each cooldown effect becomes a timer at `Spec.Duration x scale`, then
  `RemoveActiveGameplayEffect`. Scale > 1 not supported.
- Dash: `ACharacter::LaunchCharacter(facing yaw x speed, XY override)`. Effects: `MakeEffectContext` +
  `BP_ApplyGameplayEffectToSelf` (GameplayAbilities functions aren't linked: call via `GetFunction` + `Params::`).
  Class names resolve with `UObject::FindClassFast` (slow: cached per name).
