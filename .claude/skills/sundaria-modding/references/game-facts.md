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

hub (village, world "Hub"; verified in game 2026-10-08):
  camera_manager: BP_PlayerCameraHub_C; view target = a placed camera actor (BP_LobbyCamera_Hub_C on the main view, plain CameraActor on others)
  view_switching: BP_PlayerController_Hub_C::Set_View / ViewStack; buildings are trigger-volume buttons (BP_GameState_Hub_C::TriggerVolumeButtons)
  camera_override: camera-actor view targets skip BlueprintUpdateCamera → move the view-target actor itself (K2_SetActorLocationAndRotation, game thread), restore on exit
  map: fully modelled 3D town, walkable-looking from every side; only the class hall is unfinished (free-camera survey)
  collision: houses are `*_HUB` meshes with NO collision shapes (simple=0); props (stairs, tables) have shapes but collision off
  colliding_twins: `/Game/Environments/HumanTown/Meshes/<name without _HUB>` (verified in game 2026-10-08: inn houseLrg_2floors_rooms 276 shapes, houseMedT_1floor_empty 74, houseSm_1floor_2rooms 42); core adds them as hidden proxies over the hub copy. gr_bridgeA has no twin
  hero: BP_Biped_*_Player_C stands at PlayerStart (6787 19430), already possessed by the hub controller, movement mode None
  npcs: 13 NPC_* characters, each with a BP_TriggerVolumeButton_Character_C click zone next to it
  keys_taken: F8 = game's HUD toggle

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

items:   # features/item-sort; VERIFIED in game 2026-10-08 unless marked
  inventory: local PC IsA ABP_PlayerControllerOnline_C → InventoryItemContainerComponent @0x6F0, InvManagerComponent @0x6F8
  bank: ABP_PlayerControllerOnline_C::ItemContainerStorage @0x6E8 (UBP_ItemContainerStorage_C); Items 0 until the bank is opened (in town); features/inventory/shared/read.cpp keeps the last live read
  container: UBP_ItemContainerComponent_C::Items @0x128 (FBP_ItemStruct: SpecID, ContainerSlot, ContainerType, grade, level, ChangedID @0x14); one component holds several EItemContainerType values (bag 0 `DefaultContainer`, equip 1 `EquipContainer`, temp loot 8)
  identity: ChangedID is rewritten for every item on each reorder; SpecID repeats (122 distinct over 145 bag items) → key = (SpecID, level)
  stats: ItemStatList is sparse; use GetItemAttributeSet(slot, type, false) (game thread) → UArchonAttributeSet_Secondary, 128 float props read by reflection (FProperty::Offset); weapon attack stat names Map / RAP / SP
  spec: UArchonSpecManager::mLoadedSpecMap @0x28 (TMap<int32, UArchonSpec*>), every non-CDO manager; equipSlot @0x90
  enum_names: BP enums are UUserDefinedEnum: Names (NewEnumeratorN) + DisplayNameMap @0x60 (FName → FText); EnumNames() in item-sort.cpp
  ranged_melee: EWeaponDamageType names are Slash/Crush/Pierce; WeaponAnimationType is the animation (a Wand is Club); use FBP_WeaponItemSpecStruct::mWeaponType (FName: Wand, GreatBow, HeavyCrossbow, Orb …) → AttackOfWeapon() in features/inventory/shared/model.hpp
  display_name: spec ItemSpecCommonData / ItemArmorSpecData / WeaponItemSpecData .mDisplayName @0x0 (FText)
  sell_salvage: sell = BP_InvManagerComponent_C::Request_SellItems(InventorySlots); salvage = BP_TradeskillsComponent_C::RequestSalvageFromInventorySlot(SlotId); per spec: BP_SpecItemBase_C::I_CanSalvage / I_IsItemSellable (UNVERIFIED in game)
  inventory_ui: bag header UWidgetitemBagHeaderMenu_C (Button_Sort @0x278, IsStorage @0x2A8); only BndEvt__Button_Sort_* passes ProcessEvent (SortItem/RequestSortItems are BP-to-BP); X key path unknown
  widget_rect: game thread only: UWidget::GetCachedGeometry → SlateBlueprintLibrary GetLocalSize + LocalToViewport(0,0 / size) = viewport pixels
  sort_apply: InvManager.ReorderItems(SlotsToMove, IsStorage) with SlotsToMove[i] = current slot of the item that goes i-th → bag reads back in that order at once and still 5 s later (server-kept). FItemSortFunctions_C::SortItems and SortItemsInternalClient return 0 entries
  sort_partial: a partial list puts the listed items first and keeps the rest in their order (slots compacted 0..n-1); cost ~0.33–0.6 ms per listed slot, moved or not (one OnItemAddedDispatcherEvent on the controller each; Server RPC flags 0x0c6000c0) → send only the prefix up to the last changed position
  vanilla_sort: the header's Sort click returns in ~2 ms; the game's reorder follows in later ticks as 2 bursts of one OnItemAddedDispatcherEvent per item. InvManager.RequestSortItems via ProcessEvent applies a deferred sort too

dungeon (#40 survey 2026-10-08; SDK unless marked "in game"; probe: features/dungeon-map `dungeon-map.probe`):
  dungeon: ABP_Dungeon_C {FloorActors (one ABP_DungeonFloor_C per floor), LevelNamesByFloor (each floor = a streamed level),
           CurrentActiveFloor, PlayerFloors[] {PlayerState, Floor}, MapDataBound[] {Origin, Extend, Rotation, DungeonSliceClass, DungeonChunk}}
  floor: ABP_DungeonFloor_C {ChunkActors, Entry, FloorNumber, bHasBeenActivated, BP_MinimapObject (the floor's static minimap piece)}
    route_ends: I_GetStairsCrumbs(StairsUp, StairsDown), I_GetEntryChunkActor; IsPointInsideLevel(point)
  room: Abp_breadslice_C (slice) {DiscoveryBounds box, NeedsToBeDiscovered, Discovered @0x36C, DungeonSliceMapData}; BecomeDiscovered on overlap
    slice_map_data: FSDungeonSliceMapData {DungeonMapTexture (soft), OffsetAdjustment, ScaleAdjustment, BoundOrigin, BoundExtend}
  links: Abp_breadcrumb_C (crumb) = attach point {CrumbType, SpawnedActor, AttachedToCrumbType}; slice.SetCrumbsAttached(floor, a, b) joins two slices
    door_crumb: ABP_BreadCrumbDoor_C {CanSpawnDoorway, CanSpawnBlockedDoorway}; stairs: ABP_BreadCrumbStairsDown_C / Up
    room_graph: generated floors join slices by attached crumbs (SDK). Static floors have none: see static_floors
  static_floors: (in game) Crypt of Horrors floors are BP_DungeonFloorStatic_C: InitialCrumbs empty, AttachedCrumb null, MapDataBound empty;
    ChunkActors = the floor's slices (entry first, stairs-down slice `*Stairs_Down*` last); ChunkSpawnedActors = its doors; floor actor location = its entry door
    slice size 6–15 k units (100–230 minimap px): a room-centre line crosses walls
  hand_built_levels: Crypt of Horrors (1_Crypt_of_Horrors) has named slices (Slice_Dungeon_COH_2B: LockedKeyDoor, LockedSwitchDoor, BP_CryptLever, DoorCrumb1..3, …⊇)
  exit: ABP_DungeonExitVolume_C {Volume box, bAllowSoloExtract, TimerDuration}; final floor: ABP_DungeonExitVolume_FinalBossFightPortal_C
  objective: quest tracker only (WidgetSingleQuestTracker_C → ABP_QuestTrackerActor_C, HandleQuestUpdated(QuestId, IsCompleted)); no objective location found
  navigation: (in game) NavigationSystemV1.FindPathToLocationSynchronously 0.16–0.32 ms for 27–49 points across floors; closed doors block it
    (the partial path ends at the next closed door, also unlocked ones); per-floor NavMeshVolume; BP_NavigationQueryFilter_Exclude_Door* filters exist (path queries can exclude doors); ABP_WayPoint_C chains are AI patrols
  doors_levers: both subclass ABP_TriggerBase_C (: AArchonTriggerBase, native, empty)
    state: mOpenCloseAnimState @0x248 (in game: 0 closed, 2 open/pulled; 1/3 unseen, likely transitions), LockStatus @0x288 (in game: 0 unlocked, 1 locked), CanBeOpened, bStartOpen
    locks: LockRules_OR[] {LockType (ELockType 0..6), Value, TargetTag}, LockTags[], ItemLockGroup[]; door I_DoorAddLockTag / I_DoorRemoveLockTag
    event: ABP_Door_C::Dispatcher_TriggerChanged_Event_0(Trigger, TriggerState, LastTriggeringPawn); OnRep_mOpenCloseAnimState; I_IsDoorOpened(bool*)
    door_classes: ABP_Door_C → ADoor_SkeletalMeshSimple_C → Door_dun_hcr_* (cageDoorA_2BKeyDoor, 2BSwitchInsideDoor, doorSecretTorchLever, HargonSwitchDoor, …⊇)
    lever_classes: ABP_DeveloperLever_C → ABP_CryptLever_C
    owner: slice child-actor components UBP_ChildActorDungeonTriggerBase_C {TriggerActor, ConfigLock (FSLockConfig)}; reach doors/levers from the slice, no world walk
    lever_to_door: (in game) the door carries the rule (LockRules_OR / child-actor ConfigLock {type 6, value = levers needed, TargetTag e.g. Everleen});
      levers carry no tags/rules. Seen link: door and its levers are trigger child actors of the same slice. Exact mechanism unverified
    boss_gate: ABP_BossFight_ProgressionBlockingDoor_C: plain actor, collision box, no state fields
  discovered: (in game) per slice: Floor_03 4A = 1 with the pawn inside, the rooms ahead 0
  co_op: ABP_Dungeon_C::PlayerFloors per player (empty solo); minimap widget tracks PartyPawns; slice discovery is an overlap (any pawn?) — unverified

boss_fights:   # features/boss-intro/signals.cpp; SDK only, UNVERIFIED at runtime until a `[boss-intro]` log line shows them
  fight_actor: ABP_BossFight_C : AArchonBossFight (native, IsA-safe); one subclass per boss (BP_BossFight_SkeletonLord_C …⊇)
  names: FightDisplayName @0x2E8, FightStartedMessage @0x300 (FText); BossActors @0x3D8 (TArray<AActor*>, Net)
  signals_through_ProcessEvent: ReceiveBeginPlay (fight), BndEvt__ArenaTrigger_…_1_ComponentBeginOverlap / BndEvt__MasterSpawnTrigger_…_0_ComponentBeginOverlap (OtherActor @0x08),
    MulticastNotifyCombatStart, MulticastNotifyFinished(bFailed) (NetMulticast: every client), UserWidget Construct on WidgetBossSplashScreen_C,
    ReceiveBeginPlay on BP_LensEffect_bossAnnouncement_C (: AEmitterCameraLensEffectBase)
  match_by: function FName ComparisonIndex (int, outlives map travel) + IsA / class-name index; no Blueprint pointer kept

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
