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
  enum_names: BP enums are UUserDefinedEnum: Names (NewEnumeratorN) + DisplayNameMap @0x60 (FName → FText); items::sdk::EnumNames(UEnum*) in features/inventory/shared/names.cpp; the UEnum of a field: reflect::Walk to it → FByteProperty/FEnumProperty::Enum (no GObjects walk)
  ranged_melee: EWeaponDamageType names are Slash/Crush/Pierce; WeaponAnimationType is the animation (a Wand is Club); use FBP_WeaponItemSpecStruct::mWeaponType (FName: Wand, GreatBow, HeavyCrossbow, Orb …) → AttackOfWeapon() in features/inventory/shared/model.hpp
  display_name: spec ItemSpecCommonData / ItemArmorSpecData / WeaponItemSpecData .mDisplayName @0x0 (FText)
  sell_salvage: sell = BP_InvManagerComponent_C::Request_SellItems(InventorySlots); salvage = BP_TradeskillsComponent_C::RequestSalvageFromInventorySlot(SlotId); per spec: BP_SpecItemBase_C::I_CanSalvage / I_IsItemSellable (UNVERIFIED in game)
  inventory_ui: bag header UWidgetitemBagHeaderMenu_C (Button_Sort @0x278, IsStorage @0x2A8); only BndEvt__Button_Sort_* passes ProcessEvent (SortItem/RequestSortItems are BP-to-BP); X key path unknown
  widget_rect: game thread only: UWidget::GetCachedGeometry → SlateBlueprintLibrary GetLocalSize + LocalToViewport(0,0 / size) = viewport pixels
  sort_apply: InvManager.ReorderItems(SlotsToMove, IsStorage) with SlotsToMove[i] = current slot of the item that goes i-th → bag reads back in that order at once and still 5 s later (server-kept). FItemSortFunctions_C::SortItems and SortItemsInternalClient return 0 entries
  sort_partial: the k listed items land in slots 0..k-1 in list order; unlisted items keep their slots, holes too (ReorderItemsInternal: I_RemoveItem each → CombineStacks → I_AddItemToSlotFinal in a counted loop; SDK locals + log #91, bytecode unread). Send the prefix up to the last item ranked i that is not in slot i (vanilla SortItemsInternalClient: NotEqual(slot, i), lStopAtSize); comparing with the current slots instead never closes holes (#75, #91). Cost ~0.33–0.6 ms per listed slot, moved or not (one OnItemAddedDispatcherEvent on the controller each; Server RPC flags 0x0c6000c0)
  vanilla_sort: the header's Sort click returns in ~2 ms; the game's reorder follows in later ticks as 2 bursts of one OnItemAddedDispatcherEvent per item. InvManager.RequestSortItems via ProcessEvent applies a deferred sort too

characters:   # heroes of the account (#102); `just game get` on host 2026-10-09 unless marked; features/inventory/char-snapshot
  id: hero slot index; no GUID. pc.AccountComponent (UBP_PersistentPlayerAccount_C, persistent path `accountinfo`, not per hero) ActiveHeroSlot @0x1E8   # verified: `just game get pc.AccountComponent 0`
  summaries: AccountComponent.AccountData (FSPersistentAccountData @0x128).HeroSlots = TArray<FSPersistentAccountHeroSummary> (0x110), one per slot incl. deleted ones (IsDeleted); every hero's, loaded with the account   # verified: `just game get pc.AccountComponent.AccountData.HeroSlots 2`
  summary_fields: HeroName FText @0x0, HeroRace @0x18, HeroClass EClassname @0x19 (display names via DisplayNameMap, `just data show EClassname`), HeroLevel @0x1C, HeroHeroismLevel @0x2C, PrimaryStats TArray<float> @0x40 (unnamed), AbilityMappings @0x50 (action bar: FFActionBarItemInfo ID/Type/IsPassive + SlotIndices), LearnedAbilitySpecs @0x70 (EAbilityName + Level), HeroismPoints TArray<int32> @0xE0, HiddenEquipmentBitmask @0x108. No gear
  live_mirror: ps (BP_PlayerState_C) CharacterClassName, Level, HeroismLevel of the loaded hero; PlayerNamePrivate is the platform name, not the hero name   # verified `just game get ps 0`
  gear: per hero. pc.InventoryItemContainerComponent (BP_PersistentComponent_C: kPerHero true, kPersistentPath `MainContainer`, HeroSlot @0x104, bHasLoaded @0x103); equipped = container type 1 (items above)   # verified `just game get pc.InventoryItemContainerComponent 0`
  other_heroes_gear: AccountComponent.HeroSiblingClassData @0x240 TMap<hero slot, FSHeroComponentDataMap{TMap<UClass*, FSByteArray>}> = each kPerHero component's serialized save record per hero (`just data bp BP_PersistentPlayerAccount 'SerializeSiblings|OnSerializeObjectRecordData'`). Opaque AArchonSaveGame record bytes; only AArchonSaveGame::SerializeObjectFromObjectData(object, bytes) reads them (into a live component). Format and content UNVERIFIED (bridge shows maps as {num}; test account had 1 live hero, num 1)
  save_events: AArchonSaveGame::OnArchonObjectSavedForUser / OnArchonObjectLoadedForUser (PlayerController, persistentPathName, …) are bound in BP_ArchonSaveGame_C UserConstructionScript to *_Event_0 → ProcessEvent sees them (`just data events BP_ArchonSaveGame`). Firing on equip/level-up and their cadence UNVERIFIED in game (60 s idle trace: none)

dungeon (#40 survey 2026-10-08; SDK unless marked "in game"; probe: features/dungeon/shared/probe.cpp, trigger `dungeon-map.probe`):
  dungeon: ABP_Dungeon_C {FloorActors (one ABP_DungeonFloor_C per floor), LevelNamesByFloor (each floor = a streamed level),
           CurrentActiveFloor, PlayerFloors[] {PlayerState, Floor}, MapDataBound[] {Origin, Extend, Rotation, DungeonSliceClass, DungeonChunk}}
  floor: ABP_DungeonFloor_C {ChunkActors, Entry, FloorNumber, bHasBeenActivated, BP_MinimapObject (the floor's static minimap piece)}
    route_ends: I_GetStairsCrumbs(StairsUp, StairsDown), I_GetEntryChunkActor; IsPointInsideLevel(point)
    activation: (in game, Crypt) FloorActivation box around the floor's entry room; its BeginOverlap (BndEvt__FloorActivation_…_306)
      fires at the top of the floor above's stairs: the stairs slice (`*Stairs_Down_Five_Levels*`, floor above) lies inside the next
      floor's activation box, so "pawn inside a room" names the floor above all the way down. I_SetFloorActivated: called from BP script
      (BP_Dungeon), not seen by a ProcessEvent listener (unverified); CurrentActiveFloor = the last activated floor
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
    (the partial path ends at the next closed door, also unlocked ones: seen on floor 3; on Crypt floor 1 a closed unlocked cage door did NOT cut it); partial test: NavigationPath.IsPartial via umg::CallNative (unverified in game) or end > 300 from the goal; per-floor NavMeshVolume; BP_NavigationQueryFilter_Exclude_Door* filters exist (path queries can exclude doors); ABP_WayPoint_C chains are AI patrols
    navmesh_runtime: (in game, read-only memory read 2026-10-09) bGenerateNavigationOnlyAroundNavigationInvokers 0, one nav data, RuntimeGeneration
      DynamicModifiersOnly, TileSizeUU 1000: prebuilt for the whole floor, only modifiers (doors) change; the invoker component on BP_CharacterBase does nothing
    partial_end: a partial path ends at the explored point nearest the goal in a straight line, not at what cut it
    search_budget: (in game) RecastNavMesh-Default DefaultMaxSearchNodes 2048, CellSize 19; ~490 NavModifierVolume (NavArea_Null, ~7×8 m clutter
      cut-outs) per Crypt floor fragment the mesh. A long query runs out of nodes inside one connected area and comes back partial: Crypt
      floor 3 entry → stairs stopped at (84265,87457,-15961) on two runs, no door within 111 m, while a query from the room's lower level
      reached the closed cage door and the lower level connects to the upper one. Long routes: features/dungeon/shared/route.hpp (aims at
      the next room's navmesh point when a leg stops short)
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
  state: Stage @0x3C8 (EBossFightStage, uint8, enumerators unnamed in the dump; logged raw on `camera start`: values UNVERIFIED), IsFinished(bool*) (BP, unused)
  boss_alive: AArchonCharacter::mDead @0x6B4 bit0 (Net) + sampled CurrentHealth with max > 0 (#101)
  ally_test: AArchonCharacter::IsEnemyFor(AController*) native UFunction, game thread (#100: adds frozen only if enemy of the local controller); friendly summons e.g. NPC_Friendly_Karra_ItemSetSummon_C
  facing: actor yaw = RootComponent RelativeRotation.Yaw (unattached root); `[boss-intro] facing:` logs it with the mesh's relative yaw (#99, mesh offset UNVERIFIED)

loot:   # features/loot/shared (track.cpp); seen in game 2026-10-08 (Crypt of Horrors) unless marked
  classes: ABP_WorldSingleItemtLoot_C (floor items, weapon racks *_SwitchableMesh_COH_WeaponRacks_C, destructible bags) and ABP_WorldLootBase_C (chests, boss chests, tombs BP_dun_hcr_tombA_COH_C); both : ABP_TriggerBase_C
  finding: World → Levels[] → Actors[] lists only the streamed-in part of a dungeon (1 to 7 loot actors at a time); streamed-in levels fire each actor's ReceiveBeginPlay through ProcessEvent (overrides are distinct UFunctions: match by FName, not pointer)
  item_unlooted: LootIsReady @0x484 && !AActor::bHidden; grade guess ItemGradeModifier @0x510 (weapon racks read 1; whether it is the item's real grade: unverified)
  chest_unlooted: mOpenCloseAnimState @0x248 == 0 (an opened boss chest reads 2) && ItemFactoryComponent @0x418 → UBP_AffixItemFactory_C::IsLootReady @0x109 (opened chest: 0)
  chest_items: ItemContainerComponent @0x410 → Items (a closed tomb read 2 items, max grade 3; opened boss chest 0)
  vanilla_cues: no idle effect on chests (LootParticleSystem null on boss chest, tomb); weapon racks carry LootParticleSystem template P_ky_trail_ice, inactive; rim shader UseRimShader 1, Visualize 0 at rest (the component re-decides Visualize on its own tick)
  grade_colours: GetItemColorForGrade (linear) → sRGB: 0 188,188,188 · 1 white · 2 0,255,0 · 3 0,89,255 · 4 220,37,245 · 5 255,237,0 · 6 255,16,0 · 7 0,255,255
  occlusion: LastRenderTimeOnScreen @+0x290 of the rim component's RenderComponents (UBP_RimShaderComponent_C @0xD8: the loot's visible meshes, 1 each seen), compared with the newest character render time. CachedPrimitiveComp @0x3E8 is never rendered (reads -1000)
  grade_rank: EItemGrade (UserDefinedEnum, pak `DataTables/Item/ItemGrade/EItemGrade`) 0 Poor · 1 Common · 2 Superior · 3 Rare · 4 Epic · 5 Masterpiece · 6 Legendary · 7 Eternal: enum order = rank, so max() = best
  floor_item_grade: ItemGradeModifier @0x510 + ItemGradeModifier_Cap @0x581 are roll modifiers (ExposeOnSpawn), next to LootItemSpecID @0x46C / LootItemLevel @0x490; no rolled-grade field on the actor (true grade before pickup: unverified)
  rim_outline: owner's StencilLayer (BP_TriggerBase default EHighlightStencil::LootableColor = 2, StencilDistance 250) → UBP_RimShaderComponent_C::UpdateParameters → SetCustomDepthStencilValue on RenderComponents → post-process MI `Marketplace/Outliner/Materials/M_Instance_Outliner_DoS` (parent M_Parent_Outliner): ONE fixed Fill/OutlineColor per slot S1..S7 (S2 fill FFC100 outline FF4D00; S3 E4E4D4/E4E49D; S6 D4D431/FFFF3D …⊇). The yellow loot outline is that slot colour: global per stencil, no grade channel. slot ↔ stencil value mapping: unverified
  stencil_names: EHighlightStencil 0 LivingPlayer · 1 DownPlayer · 2 Lootable · 3 DeadPlayer · 4 DetectedNPC · 5 NPCQuestGiver · 6 DefaultNone · 7 NPCQuestCompletion

loot_fx:   # pak, offline (`just data`, 2026-10-09); none spawned or seen in game by us
  sparkle_like_cascade: fx_BossChest_Glow (FX/Environmental: light shaft + energy-wave mesh + flare + motes, lifetime 3–8 s, ±400 cm box: a big chest glow), P_ky_hit_shine (AdvancedMagicFX12: 350-unit shockwave + star dust burst: a hit), P_ky_trail_shine / _shineDust (spawn per unit moved: trails), dun_cas_lootPile_activateA (mesh debris on activate), fx_fireFlies, fx_env_flowerPollen_Glow …⊇
  colour_params: no Cascade system in Particles/FX/Niagara/VFX paths has a colour DistributionVectorParticleParameter (the ones found drive beam targets / mesh rotation); Niagara User.Color only on NS_Skill_Portal*, NS_Ability_Avatar / Sacrifice / ShadowCloakA, NS_ChristmasLights_Hanging (User.Color_1/_2) …⊇ (search: `just data grep 'DistributionVectorParticleParameter|User\.' --in '(Particles|FX|Niagara|VFX)/'`, capped 5000 exports)
  consequence: no game system shows a grade-coloured idle sparkle; tinting one would need a colour parameter it does not expose. idle-loot draws its motes as an overlay (#27)

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
  - input queue (read from bytecode, `just data bp BP_PlayerControllerGame`): `InpActEvt_Ability<N>` only `EnqueueAbilityInput` into `SharedInputs_PressesReleases`; `ReceiveTick → FlushInputs → ProcessAbilityInput → ASC.K2_AbilityInputPressed` runs the press on the next tick. `ProcessAbilityInput(press)` first calls `K2_AbilityInputReleased` for every other slot (pressing B releases a held A). A start blocked by the lock raises `K2_OnAbilityFailed(Handle, Ability, FailureReason ∋ AbilityFailureReason.AnimLock)` on the controller (native event → ProcessEvent, unverified in game), which stores `lastFailureAbilityInputID`; `FlushInputs` retries it every tick until it starts. `RetriggerDelayEmptyInputBuffer` = 1 s without input → release all, clear queue. `bDisableInputQueue` @0xCB1, `DodgeQueued` @0xCB0.
  - `CheckAnimLock()` returns **true when there is no lock** (`K2_CanActivateAbility` fails with AnimLock when it is false; `DebugAnimLock` prints "client has no Animlock tag" for true).
  - lock set by `K2_CommitExecute` (`ApplyAnimLockEffect` if `HasAnimLockOnActivate`, with the cooldown commit); `K2_OnAbilityCanceled` (BP_GameAbilityBase) = `MontageStop(-1)` + `RemoveAnimLock`.

ability_hits (verified in game 2026-10-08, dump removed after: read the montage live instead):
  hits per cast = notifies in the playing montage: UAnimSequenceBase::Notifies[].Notify IsA UBP_GameplayAnimNotify_C, mGameplayAnimNotifyType {ApplyEffect 0, ShootProjectile 1, AnimLockStart 2, AnimLockEnd 3}; hits = count(0)+count(1). SlotAnimTracks notifies not counted (unverified whether used)
  playing montage: local AArchonCharacter->mAbilitySystemComponent->LocalAnimMontageInfo {AnimMontage, AnimatingAbility}
  example: BP_GameAbility_RapidShot_C, Mon_RapidShot_H_M_Crossbow2H: 1.65 s, ShootProjectile ×8 → 8 hits
  landed_projectile_hit: UArchonGameplayAbility::OnProjectileHit is a BlueprintEvent, but a ProcessEvent listener matching it (by FName, obj = the animating ability) counted 0 over ~90 archer casts in game (#81, 2026-10-09); cause (pak bytecode, `just data calls BP_ProjectileBase::ReceiveHit`): ReceiveHit (ProcessEvent) → ubergraph @842 OnServerExplode (server only, IsServer @811) → ServerApplyGameplayEffectsOnExplode @1998 `ability.OnProjectileHit` as EX_LocalVirtualFunction = script VM CallFunction, never ProcessEvent. Landed hits = new LastTakeHitInfo records with PawnInstigator = hero (core Sample.hitBy); features/cast-indicator
  wind_up: first hit notify time - UAnimInstance::Montage_GetPosition(m), / Montage_GetPlayRate(m) (UFunctions, game thread); features/cast-indicator (#92, unverified in game)
  hold:   # from the pak (just data bp/show BP_GameAbility_ShootArrow, Mon_DeadlyAim_H_M_Crossbow2H), 2026-10-09
    abilities: BP_GameAbility_ShootArrow_C children with CanHold @0x9E0 true: PoisonArrow, Hemlock, DeadlyAim, ParalysingShot (AimedShot: HoldToRepeatAbility instead)
    montage: Pull (0-1.07 s) → Hold (NextSection = Hold: loops, no hit) → Shoot (2.40, ShootProjectile at its start); ParalysingShot has one section (HoldAnimInfo None), shot at 1.07
    release: K2_OnInputReleased → InputReleased=true → PlayCustomAnimation(ReleaseAnimInfo @0x9B8 = "Shoot"): in Hold = Montage_JumpToSection, during Pull = Montage_SetNextSection(Pull → Shoot)
    fire: SpawnProjectile → SpawnedArrow @0x9D4 = true → K2_EndAbility (AnimatingAbility clears the same frame); SpawnedArrow/InputReleased reset in K2_OnActivateTasks
    not_hold_levels: mHoldLevel is set to 1 on reset and only climbs in the cast-time path (mStartedCast, AutoActivateOnCastFinish false); archer abilities logged "hold 1/5" every cast, so hold levels are not the archer hold
    consumer: features/cast-indicator timeline.hpp (Timeline::Hits/Ahead/Crossed)
  salvo: UBP_GameAbility_Salvo_C has ConeDegree @0x91C, HitActors @0x940, LastApplyEffectId @0x934 (cone trace, de-dup per target): its 20 hit notifies are not 20 hits per target; logged "hits 20, landed 4/9" (#81). Grouping by ApplyEffectID unverified: `[cast-indicator] notifies <montage>` log line
  landed_vs_records: one record per target per frame (same-frame hits sum), several targets = several records: landed counts records, not arrows
  landed_0: "RapidShot ... hits 8, landed 0" after a reload (2026-10-09, 1_Crypt_of_Horrors) came with no enemy HP change in the [hpt] trace: shots at nothing, not a regression
  montage_end: ASC.LocalAnimMontageInfo.AnimatingAbility clears when the montage ends (UE 4.27 GAS; unverified here); PlayBit flips per play, so a recast of the same montage is a change
  montage per ability/weapon: soft ptrs in UBP_GameAbilityBase_C::{AnimSkeleton_OverrideWeaponAnims, OverrideAnims}, UBP_GameAbility_WeaponMontage_C::OverrideWeaponAnims{Left,Right}; their weak index is unset → resolve a loaded montage by object name; only montages of equipped weapons are loaded (300/1339)
  sdk_quirk: SDK TMap iteration/operator[] fails to compile (SetElement::Value private) → read raw: Data ptr at +0, stride = sizeof(TPair)+8, skip free slots with IsValidIndex

- Paks: Windows build ships `Archon/Content/Paks/Archon-WindowsNoEditor.pak` (~8 GB, pak V11) with an **encrypted index** (repak: "pak is encrypted but no key was provided"); the native Linux pak's index was not encrypted. A `~mods/<name>_P.pak` we build ourselves needs no key; listing/extracting the game's assets does.
- Ground movement (`features/ow-movement`, core `game::ApplyGround`): `UCharacterMovementComponent` MaxAcceleration @0x1A0, BrakingDecelerationWalking @0x1B4, GroundFriction @0x16C, BrakingFrictionFactor @0x1A8, JumpZVelocity @0x158, Velocity @0xC4 (UMovementComponent). Unverified whether sprint/abilities rewrite them.
- Console variables (`features/graphics`): `UKismetSystemLibrary::ExecuteConsoleCommand` / `GetConsoleVariableFloatValue` (static UFunctions, game thread). Which cvars the shipping build accepts: unverified.
- Movement during abilities (#36): `UArchonCharacterMovementComponent` adds only prone fields; no cast-slow attribute found. Suspects: anim-lock GE `BP_GameplayEffect_AnimLock` (+`_VeryLong`), root-motion montages, `IgnoreMoveAndAbilityInput`. Needs a debug-probe run.

weapon_types (SDK only, values unread in game; probe: features/weapon-probe, `weapon-probe.probe` → `dos-tool-weapons.yaml`, issue #77):
  per_type_stats: UBP_SpecItemWeapon_C::GetWeaponTypeStat → FSWeaponTypeStats {AnimationType FName, DamageModifier, EstimatedAnimationSpeed, AttackSpeed}; bow/crossbow numbers come from here
  ability_gate: UBP_GameAbilityBase_C::WeaponTypesQualifier (TArray<EWeaponType> @0x5E0) + CheckWeaponTypeRequirement
  hold: UBP_GameAbility_ShootArrow_C::CanHold @0x9E0, HoldAnimInfo/ReleaseAnimInfo; mHoldLevel/mHoldInterval/kMaxHoldLevel on GameAbilityBase
  speed: ShootArrow/RapidShot WeaponTypePlayRate TMap<EWeaponType,float>; Salvo PlayRateScaleCrossbow @0x918
  forum (steamcommunity.com/app/587520/discussions/0/4030223221276280137/, user posts, not verified): bow skill fires instantly, no draw; only a Ranger has an aim animation on one skill; Rogue can slot "shoot arrow"
  forum (…/5946473955238289588/): two 1H crossbows give magic pen (user claim, unverified)

dps_probe (#89, Alpha optIn, features/inventory/dps-probe): file trigger `dps-probe.probe` next to the exe -> `dos-tool-dps-state.yaml`
  content: hero + 3 nearest non-player characters: every float of each SpawnedAttributes set (Primary/Secondary/Heroism/Status/Config), hero's equipped items with rolled stats (items::io::Read)
  not in it: tables, curves, bytecode (offline pak extractor), active gameplay effects (buffs)   # unverified in game
