# Game facts (Dungeons of Sundaria, Steam 587520)

Verify any offset with `scripts/sdk.py` before relying on it — a game patch moves them.

```yaml
engine: UE 4.27.2, changelist 49798, project "Archon"   # Dumper-7 dump header
exe: Archon/Binaries/Win64/Archon-Win64-Shipping.exe     # Steam launches it directly; no root launcher
platform_for_mods: Windows build via Proton (native Linux build cannot load DLL mods)
launch_option: WINEDLLOVERRIDES="winmm=n,b" %command%     # winmm.dll = Ultimate ASI Loader v9.7.4 (official dinput8.dll renamed)
proton_prefix: ~/.steam/steam/steamapps/compatdata/587520/pfx

read_paths:   # all plain memory reads, safe from the render thread
  world: UWorld::GetWorld() → Levels[i] → Actors[j]
  local_player: World → OwningGameInstance → LocalPlayers[0] → PlayerController (→ Pawn)
  characters: actor IsA AArchonCharacter (: ACharacter : APawn); 107 subclasses in dump, every enemy/boss/player
  is_player: APawn::PlayerState != null
  health: AArchonCharacter::mAbilitySystemComponent → SpawnedAttributes[] → IsA UArchonAttributeSet_Status → CurrentHealth (float)
  max_health: UArchonAttributeSet_Secondary::Health @0x30 → Sample.maxHealth; health-bars uses max(it, peak HP seen)   # unverified at runtime; Health_Bonus @0x114 may be additive
  level: UArchonAttributeSet_Status::CurrentLevel (float) → Sample.level   # shown as "Lv.N" on health bars; unverified in game
  position: RootComponent->RelativeLocation = capsule center (root unattached ⇒ relative == world)
  camera: PlayerController → PlayerCameraManager → CameraCachePrivate.POV {Location, Rotation, FOV(horizontal)}
  gameplay_camera: PlayerCameraManager IsA ABP_PlayerCamera_C (only in gameplay; menus use another class → check IsA first)
    fov_source: APlayerCameraManager::DefaultFOV
    distance_source: ABP_PlayerCamera_C::kInitialOrbitDistance   # writing TargetArmLength flickers
    collision: USpringArmComponent::bDoCollisionTest

movement: ACharacter::CharacterMovement @0x288 (UArchonCharacterMovementComponent)
  fields: {AirControl: 0x1C4, AirControlBoostMultiplier: 0x1C8, AirControlBoostVelocityThreshold: 0x1CC,
           FallingLateralFriction: 0x1D0, BrakingDecelerationFalling: 0x1B8, GravityScale: 0x150, JumpZVelocity: 0x158}
  server_simulated: write to EVERY player character; in co-op the host's values win, clients get corrected
  unknown: whether abilities/dodge reset these each frame

last_hit:   # verified in game 2026-10-08 (host): plain memory read on the render thread, no hook
  field: AArchonCharacter::LastTakeHitInfo @0x588 (FTakeHitInfo, replicated)
  DamageTypeClass @+0x08: one class per ability for most skills (Range_AimedShot_C vs basic Range_C, Range_ToxicArrow_C, Magic_Void_C)
  ActualDamage @+0x00: HP lost by that hit (clamped to remaining HP on a kill)
  EnsureReplicationByte @+0x28: changes per record; often +2 per hit
  PawnInstigator @+0x10 (weak ptr): who hit; NPC hits on the player carry the NPC
  same_frame: the game SUMS same-frame hits from one instigator into ActualDamage, type = the last one
    (Void 21.9 then AimedShot 175 = 153.1 arrow + 21.9 void; HP dropped 175)
  timing_vs_hp: render-thread sampling sees the record and the HP drop in either order, and one hit's drop can split over 2 frames
  dots: ticks are their own classes (BP_DamageDot_C generic, BP_DamageDOT_Poison_C), ~1.0 s apart; Magic_Poison ticks as Magic_Poison
    NOT linked to the ability that applied them: only instigator + DoT class known here
  zero_damage: Magic_C records with ActualDamage 0 on the player (self-cast) happen
  consumer: core/combat.hpp ledger (records claim HP loss) → damage numbers per ability, coloured by element

items:   # features/item-sort; read from headers, all UNVERIFIED at runtime (check the [item-sort] log lines)
  inventory: local PC IsA ABP_PlayerControllerOnline_C → InventoryItemContainerComponent @0x6F0, InvManagerComponent @0x6F8
  bank: UBP_InvManagerComponent_C::PlayerPersistentComponent @0x148 (UBP_ItemContainerStorage_C), else ItemStorage @0xC0 → PlayerComponent @0x238
  container: UBP_ItemContainerComponent_C::Items @0x128 (FBP_ItemStruct: SpecID, ContainerSlot, ContainerType, grade, level); one component holds several EItemContainerType values
  stats: same component ItemStatList @0x1D8, joined on (ItemSlot, ItemContainerType) → SingleStatList {EStatType, float}
  spec: UArchonSpecManager::mLoadedSpecMap @0x28 (TMap<int32, UArchonSpec*>), every non-CDO manager; weapon spec WeaponAnimationType @0x158, WeaponDamageType @0x159; equipSlot @0x90
  enum_names: BP enums are UUserDefinedEnum: Names (NewEnumeratorN) + DisplayNameMap @0x60 (FName → FText); EnumNames() in item-sort.cpp
  ranged_melee: EWeaponDamageType display name (3 values, expected Melee/Range/Magic), fallback EWeaponType name; keyword rule AttackIn() in item-sort.hpp
  sort: SortItemsInternalClient(EItemSort, IsStorage) → SlotsToMove (game's format) → permuted by profile → ReorderItems(SlotsToMove, IsStorage); format plain vs encoded decided per call, encoded decoded with FItemContainerFunctions_C::ConvertCompressedItemSlot

combat_log (unused so far; crits + own-vs-party damage; whether its delegates pass ProcessEvent: UNVERIFIED):
  delegates: AArchonCharacter::OnCombatLogGeneratedDelegate_Offense / _Defense
  payload: FCombatDetailDamage {FinalDamage, BlockedDamage, CritLevel, ResistedDamage, MitigatedDamage_Armor, bIsHeal}
  cost: needs a ProcessEvent/delegate hook on the game thread, not the Present hook

damage_types: UArchonGameplayEffect::mDamageTypeClass (TSubclassOf<UDamageType>, the Effect arg of the combat-log delegate)
  classes: `sdk.py subs UDamageType` → UBP_DamageType_Magic_Ice_C, _Burn_, UBP_DamageDOT_Poison_C, …⊇
  colours: features/damage-numbers/colors.hpp (name → element → colour, tested)
attack_type: EGameplayAttackType {Melee, Range, Magic} via UArchonGameplayEffect::GetAttackType (UFunction: game thread only)
ProcessEvent: Offsets::ProcessEvent (Basic.hpp) — hookable with MinHook; see gotchas before locking in the detour
damage_numbers_source_today: per-frame CurrentHealth diff = amount; LastTakeHitInfo = which ability/element/instigator; no crits
- `UArchonAttributeSet_Secondary::Health` is NOT max HP: it exceeds `CurrentHealth` on unhit enemies. Max HP = peak `CurrentHealth` seen.
- On-screen test (behind wall = not drawn): `UPrimitiveComponent::LastRenderTimeOnScreen` at `+0x290` of `ACharacter::Mesh` (inside Dumper-7 `Pad_288` after `BoundsScale`; 0x288 LastSubmitTime, 0x28C LastRenderTime; found by scanning floats that advance with time). Compare against the newest value over all characters, not wall time. `seen=` in the debug-probe `[hp]` log.
- Ability phases (used by `features/input-feel`, unverified in game):
  - press: `ABP_PlayerControllerGame_C::InpActEvt_Ability<1..12>_K2Node_InputActionEvent_*` (2 per slot, press + release, param `FKey`; tell them apart with `IsInputKeyDown`). These go through ProcessEvent; `ProcessAbilityInput(Pressed, EAbilityInputName, Ignored)` is called from inside the BP VM, so it does not.
  - animating ability: `UAbilitySystemComponent::LocalAnimMontageInfo.AnimatingAbility` (@0x788 + 0x20).
  - "out": `UBP_GameplayAnimNotify_C::Received_Notify(MeshComp, Anim)` with `mGameplayAnimNotifyType` @0x38 = `ApplyEffect` or `ShootProjectile` (`AnimLockStart`/`AnimLockEnd` also exist). The notify also carries `HitStop` @0xA6 and `mAnimLockExpireTime` @0x60.
  - lock: `UArchonAbilitySystemComponent::CheckAnimLock` / `RemoveAnimLock` / `ApplyAnimLockEffect` (GE `BP_GameplayEffect_AnimLock`); cancel: `UGameplayAbility::K2_CancelAbility`.
  - input queue: `ABP_PlayerControllerGame_C::bDisableInputQueue` @0xCB1, `DodgeQueued` @0xCB0. Queued presses fire after the lock ends (`RetriggerDelayEmptyInputBuffer`).

ability_hits (verified in game 2026-10-08, dump removed after: read the montage live instead):
  hits per cast = notifies in the playing montage: UAnimSequenceBase::Notifies[].Notify IsA UBP_GameplayAnimNotify_C, mGameplayAnimNotifyType {ApplyEffect 0, ShootProjectile 1, AnimLockStart 2, AnimLockEnd 3}; hits = count(0)+count(1). SlotAnimTracks notifies not counted (unverified whether used)
  playing montage: local AArchonCharacter->mAbilitySystemComponent->LocalAnimMontageInfo {AnimMontage, AnimatingAbility}
  example: BP_GameAbility_RapidShot_C, Mon_RapidShot_H_M_Crossbow2H: 1.65 s, ShootProjectile ×8 → 8 hits
  montage per ability/weapon: soft ptrs in UBP_GameAbilityBase_C::{AnimSkeleton_OverrideWeaponAnims, OverrideAnims}, UBP_GameAbility_WeaponMontage_C::OverrideWeaponAnims{Left,Right}; their weak index is unset → resolve a loaded montage by object name; only montages of equipped weapons are loaded (300/1339)
  sdk_quirk: SDK TMap iteration/operator[] fails to compile (SetElement::Value private) → read raw: Data ptr at +0, stride = sizeof(TPair)+8, skip free slots with IsValidIndex

- Paks: Windows build ships `Archon/Content/Paks/Archon-WindowsNoEditor.pak` (~8 GB, pak V11) with an **encrypted index** (repak: "pak is encrypted but no key was provided"); the native Linux pak's index was not encrypted. A `~mods/<name>_P.pak` we build ourselves needs no key; listing/extracting the game's assets does.
- Ground movement (`features/ow-movement`, core `game::ApplyGround`): `UCharacterMovementComponent` MaxAcceleration @0x1A0, BrakingDecelerationWalking @0x1B4, GroundFriction @0x16C, BrakingFrictionFactor @0x1A8, JumpZVelocity @0x158, Velocity @0xC4 (UMovementComponent). Unverified whether sprint/abilities rewrite them.
- Console variables (`features/graphics`): `UKismetSystemLibrary::ExecuteConsoleCommand` / `GetConsoleVariableFloatValue` (static UFunctions, game thread). Which cvars the shipping build accepts: unverified.
- Movement during abilities (#36): `UArchonCharacterMovementComponent` adds only prone fields; no cast-slow attribute found. Suspects: anim-lock GE `BP_GameplayEffect_AnimLock` (+`_VeryLong`), root-motion montages, `IgnoreMoveAndAbilityInput`. Needs a debug-probe run.
