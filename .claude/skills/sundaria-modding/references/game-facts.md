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

loot_events:   # `just data events BP_WorldSingleItemtLoot|BP_WorldLootBase`, `just data callers OnLootReady|CollectingLoot` (2026-10-09); firing in game unverified
  item_pickup: Dispatcher_TriggerChanged (bound in ReceiveBeginPlay) → OnTriggerChanged (ProcessEvent) → CollectingLoot (script VM); OnLootReady(isReady) toggles LootParticleSystem, reached by script calls only on items (OnRep_LootIsReady on clients)
  chest: ItemFactoryComponent.OnLootReady bound → BP_WorldLootBase_C::OnLootReady (ProcessEvent); OnItemFactoryEmptied bound event (BndEvt__…K2Node_ComponentBoundEvent_1…)
  multicast: MulticastPlayPickupSound is NetMulticast: a host's own BP call runs it in the script VM (no ProcessEvent); clients receive it via ProcessEvent

loot_fx:   # usable templates + API: fx.md (`just data fx`). pak, offline (`just data show <asset>`, 2026-10-09; summaries of all 664 ParticleSystem assets); in game: unverified unless marked
  vanilla_on_loot: rare chest BP_TreasureChest_Rare = fx_BossChest_Glow; boss chests fx_weapon_iceMist / fx_weapon_holyLvl2 / fx_weapon_holyMist; weapon racks LootParticleSystem P_ky_trail_ice (inactive); loot pile dun_cas_lootPile_activateA (coin burst on collect) …⊇
  candidates_rejected: fx_env_flowerPollen_Glow (1 emitter, 450 cm cylinder, life 25–50 s, warmup 50 s: a room-wide haze), fx_BossChest_Glow (4 emitters: flare + light + motes, a big glow), P_ky_hit_shine / P_ky_trail_shineDust (bursts, loops 1), P_Elemental_ice_ambient_01 (smoke + 30–50/s sparkles), fx_fireFlies (+ a dynamic-light emitter) …⊇
  chosen_template: Environments/HumanProps/Magic/Particles/hp_mag_alchemyOrb_fireflies (features/loot/idle-loot): 1 sprite emitter, burst 5 at t 0, infinite life, Orbit offset 0–20 cm, rotation rate 0.1–0.25, ColorOverLife white, DynamicParam ThoraxFlickerRate 0.05–5 (spawn-time random), material MI fx_fireFlies_Full (parent fx_fireFlies, BLEND_Masked, bUsedWithParticleSprites)
  fx_fireFlies_params: scalar Thorax Flicker Rate, Wing Rate; vector Diffuse, Emissive Thorax (MI: FFFF00); texture Mask
  colour_params: no Cascade system has a colour DistributionVectorParticleParameter; Niagara User.Color only on NS_Skill_Portal*, NS_Ability_Avatar / Sacrifice / ShadowCloakA, Blood_VFX P_* (blood), NS_ChristmasLights_Hanging (User.Color_1/_2, spawns on a static mesh) …⊇
  colour_route: per-component material instance (UPrimitiveComponent::CreateDynamicMaterialInstance(0, null) + SetMaterial) and a vector param of the emitter's material. Particle-sprite materials with a colour vector param: fx_fireFlies (Emissive Thorax), M_GPP_RadialGlow_Tint / M_GPBAR_RadialGlow_Tint (Color A, Color B; additive radial glow) …⊇. Sparkle materials M_FlickeringSparkle_01/02/03, M_ky_starDust, fx_base_flareBASE take colour only from the particle colour (no vector param)
  cooked_material_params: `just data show <Material>` → CachedExpressionData.Parameters."RuntimeEntries" = scalars, "RuntimeEntries[1]" = vectors, "[2]" = textures (expressions are stripped)

combat_log (no use for crits):
  delegates: AArchonCharacter::OnCombatLogGeneratedDelegate_Offense / _Defense
  payload: FCombatDetailDamage {FinalDamage, BlockedDamage, CritLevel, ResistedDamage, MitigatedDamage_Armor, bIsHeal}
  broadcast_by: BP_CharacterBase_C::FAnyDamage @962/@1322 with CritLevel 0, Blocked/Resisted/Mitigated 0 always; bound only by the debug BP_CombatLogger (`just data bp BP_CharacterBase FAnyDamage`, `just data callers OnCombatLogGeneratedDelegate_Offense`)   # verified (bytecode)
damage_event:   # features/inventory/dps-probe (#111)
  hit: BP_CharacterBase_C::ReceiveAnyDamage(Damage, DamageType CDO, InstigatedBy controller, DamageCauser) on the damaged character, via ProcessEvent (AActor::TakeDamage → ubergraph → FAnyDamage)   # seen on the hero being hit: `just game trace 'ReceiveAnyDamage|OnExecute' 30` 2026-10-09; on enemies hit by the hero UNVERIFIED; server side (TakeDamage), co-op client UNVERIFIED
  crit: GameplayCue OnExecute declared by BP_GameplayCueNotifyStatic_HitImpact_C, object class BP_GameplayCueNotifyStatic_HitImpact_Critical_Damage_C on a crit (instead of _HitImpact_C / _HitImpact_Melee_C), params {MyTarget, FGameplayCueParameters}   # trace 2026-10-09: 1 crit cue of 8 HitImpact cues, 7 ReceiveAnyDamage; same tick as the damage UNVERIFIED
  crit_math: BP_GameAbilityBase CritHitLevel / GetAbilityCritLevel / GetCriHitMultiplier are BP-to-BP (no ProcessEvent)

damage_types: UArchonGameplayEffect::mDamageTypeClass (TSubclassOf<UDamageType>, the Effect arg of the combat-log delegate)
  classes: `sdk.py subs UDamageType` → UBP_DamageType_Magic_Ice_C, _Burn_, UBP_DamageDOT_Poison_C, …⊇
  colours: features/damage-numbers/colors.hpp (name → element → colour, tested)
attack_type: EGameplayAttackType {Melee, Range, Magic} via UArchonGameplayEffect::GetAttackType (UFunction: game thread only)
ProcessEvent: Offsets::ProcessEvent (Basic.hpp) — hookable with MinHook; see gotchas before locking in the detour
  function_outer: "a UFunction's Outer is the class that declares it; a call on a subclass object keeps it (BP_CharacterBase_C::ReceiveTick
    on NPC_132_E_Gnoll_Druid_C). game::On matches function FName + this Outer FName. `just game trace ReceiveTick$ 3`, verified 2026-10-09"
  rate: "game thread, 3_Grasslands dungeon, live bridge connected: ~18k calls/s over ~300 function x class pairs
    (`just game trace . 2`: 35846 calls in 2 s); local player controller ReceiveTick ~60/s. Verified 2026-10-09, one sample"
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
  - FlushInputs order (#112): a pending `lastFailureAbilityInputID` is retried alone first (then return); otherwise all queued inputs run in one call, hold-to-repeat inputs (`I_GetIsHoldToRepeat`, e.g. AimedShot; `CheckHoldToRepeatAbility` → `LastHoldToRepeatInputID`) sorted last. `K2_OnAbilityFailed` fires inside that call, so a lock removed there is taken by the next queued input in the same call (a held AimedShot), not by the blocked press. `K2_OnAbilityEnded` of another ability re-enqueues a held hold-to-repeat input (ubergraph 916–1386).
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
  salvo: UBP_GameAbility_Salvo_C has ConeDegree @0x91C, HitActors @0x940, LastApplyEffectId @0x934 (cone box sweep; HitActors de-dups knockback only, see dps_mechanics.aoe_targets.salvo_cone): its 20 hit notifies are not 20 hits per target; logged "hits 20, landed 4/9" (#81). Grouping by ApplyEffectID unverified: `[cast-indicator] notifies <montage>` log line
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
  hits (#111): while enabled, no trigger: hero's hits (damage_event above) → `hits:` per enemy class first plain hit + first crit, ≤32 classes, file rewritten on the world tick when one is added; log `[dps-probe] captured <class>`   # unverified in game

dps_mechanics:   # pak bytecode/CDOs via `just data`, 2026-10-09 (dps-sim, outer repo). Facts and locations only; values stay in the pak
  item_affixes:   # verified 2026-10-09: a 17-item char snapshot (dos-tool-chars) predicted stat-for-stat from (spec, slot, grade, level), 146/146 exact
    dispatch: BP_ItemContainerComponent::CreateItemAttributeSet switches on the spec's EnableRandomStats (ItemTable_Armor/_Weapon) → 0-3 InitAttributeSet_Random (StatPriority path), 4 fixed spec set, 5 ScalableStats, 6/8 InitAttributeSet_ScalableCraftBonus, 7 ScalableRandom; then Finalize Attribute Set   # `just data bp BP_ItemContainerComponent CreateItemAttributeSet`
    which_path: nearly every armor/weapon spec has EnableRandomStats 8 → the crafting-bonus path below; the StatPriority path (GetStatPriority, NewItem_Armor_RandStats) only serves the few 0-3 specs   # `just data table ItemTable_Armor`
    pools: FItemDataTableFunctionLibrary::GetStatCraftBonus → table by ItemGenerationVersion (0 CraftingBonus_Gen0, 1 _Gen1, 2 _Gen2, 3 CraftingBonus), row = spec ScalableCraftingTag (+GenIndex suffix when > 0, GenerateCraftBonusIndex): Mandatory1 (always), <Grade> (additional), Filler_<Grade>, Mandatory_Bonus_<Grade>; mandatory stats are removed from the other lists
    counts: per list Min + Round((Max − Min) × seed) from ITEMGRADE_Table (AdditionalAttributes / Filler / MandatoryByGrade Min/Max; GetAdditionalAttributeNumBySeed etc.); pick order additional → filler → mandatory-by-grade
    pick: index = Round(LastIndex × u) into the remaining list, then Array_Remove → no repeats; first and last entries are half as likely as the others; AddUnique into the item   # InitAttributeSet_ScalableCraftBonus @2879..6611; u from a seeded RandomStream / static seed table
    value: no roll; stat pre-value = ARMOR_EQUIVALENCY column (MAP/RAP/SP, ArmorFactor → BaseToPlate, MagicResistance → ResistElementalBaseToCloth; row = armor type name, or weapon type name with struct default 1.0 when absent) × (1 if StatTypeIgnoredDistribution else SLOT_TABLE[slot]; WeaponAny reads the WeaponLeft row)   # ScalableCraftBonus @6612..9462, GetEquipSlotDistribution
    final: Finalize Attribute Set: pre × MASTER_ITEM_STAT_TABLE[stat](item level) × max(ItemGrade.Quality, 0.7) [× ElementalMagicMod for WeaponDamage_<elem>] [× Weapon_Stats.DamageModifier for WeaponDamage] [× EndGameTier curve when ItemTier ≠ 0], in float32
    rounding: BP_ItemContainerComponent::RoundUpAttributeValues → FCeil to 1, or to 0.01 when DataTable_AttributeSetUI.InPercentageValue; float32 error can lift an exact 0.01 to 0.02 (seen on a weapon CriticalDamage_Melee)
    grades: item grade byte = EItemGrade (3 Rare, 4 Epic, 6 Legendary, …⊇); quality and pick counts by grade row of ITEMGRADE_Table (Gen1 table for generation versions 0/1)
    gap: one Legendary ring carries 5 picks from its additional list where the grade allows fewer; cause not found (upgrade path / other generation version: unverified)
  weapon_elements: BP_GameplayCombatLibrary::GetWeaponDamage loops EMagicDamageType 0..7 over the attacking weapon's attribute set and keeps only the first element with a value (array stops at length 1); ApplyTransientDamageInfoGE @8404 clones one elemental hit from it → a weapon with Fire + Nature + Holy deals only its Fire part   # verified (bytecode)
    hero_sheet: weapon elements are not summed into the hero's Secondary set (live: WeaponDamage_* 0 with elemental weapons equipped)
  attack_power: AP attribute = (1stMod × 1stStat + 2ndMod × 2ndStat + (L div maxLevel + 1) × L × LevelMod  [AttackPowerTable row <Class><Melee|Ranged|Spell>] + gear AP) × (1 + <X>Power_Bonus); damage AP term = AP / 220   # verified live to 0.01 (dual-wield Ranger L10, no class-weapon AP buff active)
  armor_mitigation: 1 − A / (A + 700 + 4 × L_target) with A = ArmorFactor (physical) or MagicResistance (magic)   # verified live: TransientDamageMultiplier_ArmorMitigation of a hero hit on a wolf and of the wolf's hit on the hero both match to 6 digits
    damage_by_attack_type: 1 + OutgoingDamageMod (+ Damage_<type>); resist_by_attack_type: 1 + target IncomingDamageMod (− Resist_<type>)   # verified live on the wolf's hit
  crit_chance: c = (0.05 + CriticalChance) × (1 + BaseCritChance_Bonus) → CriticalChance adds, BaseCritChance_Bonus scales the sum (ApplyTransientDamageInfoGE, GetCritChance)   # verified (bytecode)
  crit: crit damage multiplier reads CriticalDamage_Melee for every attack type; CriticalDamage_Range/_Spell roll on items but have no reader in the damage path (`just data bp BP_GameAbilityBase ApplyTransientDamageInfoGE` @1626..4952, GetCriHitMultiplier)   # verified (bytecode); `readers` lists only the equip copy BP_CharacterBase::UpdateSecondaryStats-CriticalDamage, GameplayAttribute reads are not indexed
  cooldown: CooldownReduction_Bonus → AbilitySystemAttributeSet.CooldownDurationMultiplier = 1 − CDR   # verified live (bridge), mapping GE not traced
    commit: BP_GameAbilityBase K2_CommitAbilityCooldown by CooldownMode (`just data bp BP_GameAbilityBase | grep CooldownMode`); duration = curve row <Class>_<Ability>.CoolDown
    throw_dagger: BP_GameAbility_ThrowDagger CDO has no CooldownGameplayEffectClass (base neither) → no cooldown; 1 projectile, 1 hit (Mon_ThrowDagger_H_M: one ShootProjectile notify)   # verified (CDO + montage)
  class_weapon_restriction: none found (readers of mAllowedClasses / AllowedClass / ClassRestriction: 0); only per ability: WeaponTypesQualifier + CheckWeaponTypeRequirement   # verified absence in Blueprints; C++ not checked
  item_use_rules:   # what lets a hero use a gear piece (#126); offline, 2026-10-10; no in-game test
    level: BP_AffixContainerEquip::I_AffixCanEmplace @393 = hero.GetLevel(false) >= ItemStruct.ItemLevel (item INSTANCE level, FBP_ItemStruct; the spec row LevelRequirement is 0 in ItemTable_Weapon)   # verified (`just data bp BP_AffixContainerEquip I_AffixCanEmplace`)
    default_attack: BP_PlayerControllerGame::AssignDefaultAbilityOnEquip (server, equipping a non-shield weapon) = tag Ability.Ranger.ShootArrow for weapon types Crossbow, Bow2H, Crossbow2H, else Ability.MeleeAttack; then ASC.FindAbilityInputIDByTag; nothing is bound when the hero owns no such ability. Only the Ranger owns ShootArrow, so ranged weapons leave every other class without a basic attack   # verified (bytecode); explains "cant even use it" (maintainer, Rogue + crossbow), not tested in game
    melee_attack: Generic/MeleeAttack WeaponTypesQualifier = Axe Club Sword Dagger Hammer2H Polearm2H Scythe2H Staff2H Sword2H Axe2H Shield Fist (no Crossbow*, Bow2H)   # verified (CDO)
    abilities: K2_CanActivateAbility → CheckWeaponTypeRequirement: empty WeaponTypesQualifier passes, else a hand's EWeaponType must be listed (per ability: Rogue abilities listing Crossbow2H still pass)
    staff: Staff2H is in MeleeAttack and most Rogue lists; no rule found that blocks a Staff for a Rogue, a high Staff score at hero level 1 is the item level rule   # unverified in game
    in_libs_dps: tables record `rangedattack ShootArrow <types>` (scripts/dps_tables.py, from the ShootArrow qualifier) + Item.level vs Build.level in detail::CanUse → ScoreItem fits=false, BestInSlot skips. Rebuild the DLL together with tables from this script (older DLL: "bad record 'rangedattack'")
    class_weapon_bonus: DA_ClassWeaponBonus_StackingEffects = per-class weapon-tag buff sets (Ranger Hunter/Skirmisher/Enforcer, …⊇), buffs not restrictions; live Ranger_Hunter buff adds AttackPower_Ranged (explains RAP above gear+base)
  granted_abilities: a live Ranger carries Warden abilities (ToxicArrow, Hemlock, HealingVapors) from Blueprints/Ability/Abilities/Warden/; curves in DataTables/Abilities/Heroism/Warden/   # verified live (ActivatableAbilities)
  aoe_targets:   # where the target count of a damage GE comes from
    projectile_radius: BP_GameAbilityBase::GetProjectileDamageRadius reads ProjectileDamageRadius (curve ref on the CDO): ExplosiveArrow FireDamage, Hemlock Poison (cloud)   # verified (CDO + reader)
    salvo_cone: BP_GameAbility_Salvo ubergraph @1386: FarDistanceTraceBoxExtent = sin(ConeDegree/2) × GetAbilityTraceRange → GetCustomBoxSweepLocations (5 boxes) @2246 → every actor in the sweep per notify   # verified; per-notify damage per target inferred
      hit_actors: HitActors is filled only by the knockback event (Array_AddUnique @2483, cleared on activate @2657) → knockback de-dup, not damage de-dup; LastApplyEffectId is read only by DemonBane   # verified (`just data readers LastApplyEffectId`)
    multishot: BP_Projectile_MultiShot spawned once per mSpawbRotateMods entry → targets ≤ projectiles   # verified (CDO)
    blasting_shot: MineCount (GetMineCount) mines, TrapSpread; explosion radius = curve row Mine.Explosion.Radius   # mine count verified, radius use inferred from row name
    traps: curve row Ranger_Trap.Radius   # inferred from row name
    poison_arrow: GE_PoisonArrow_Poison + HitDamage on projectile hit = single target; SpawnCriticalBonusAura (ubergraph @1536) spawns a crit-bonus aura, not a damage cloud   # verified
    toxic_arrow: on hit GE_ToxicArrow_HitDamage + GE_ToxicArrow_Poison_IncomingDamageUp (target takes more Nature damage) = single target   # verified (CDO)
targeting:   # features/targeting (cast mode #107, marker #104); pak bytecode via `just data bp` + `just game` on host, 2026-10-10
  ability_fields: UBP_GameAbilityBase_C mDefaultAbilityTargetingType @0x559 (EGameplayEffectTargetingType TraceAny 0 / TraceEnemy 1 / TraceFriend 2 / Self 3), mRequireValidTarget @0x558, TraceSphereRadiusOverride @0x5B4 (-1 = target actor default), bTraceSphereMulti, mTraceProfile, mSpawnProjectile, HoldToRepeatAbility, kMaxHoldLevel; range = GetAbilityTraceRange() (curve row Range)
  aim: K2_DoRadiusTargetDataTask → WaitTargetData("TraceRadius", BP_SphereTargetTrace_C) = AGameplayAbilityTargetActor_Trace: camera ray clipped to the range sphere around the hero, line trace by mTraceProfile, then SphereTraceSingleForObjects (Pawn + Destructible) from the hero along it (ported: features/targeting/shared/aim.hpp); filter drops self only   # marker matched the cast target in play; not counted over 20 casts
  friend_or_enemy: GameState.I_AreActorsEnemies(a, b) compares I_GetKingdomId; characters return AArchonCharacter::ReplicatedKingdomId @0x6C4 (player -1, wooden dummy / monsters 666; 777 and 1 are special); enemies when exactly one of the two is 666. Setter I_SetKingdomId(id, reason): `just game call @1 I_SetKingdomId '[-1, 0]'` turned a training dummy into an ally until the map reloads   # verified live
  key_to_ability: controller InpActEvt_Ability<N>_* (press + release, parm FKey) → EnqueueAbilityInput(EAbilityInputName, press, release): slots 1..6 = N-1, N+5 with AbilityModifierPressed @0xBE0; 7..12 = N-1. FlushInputs → ProcessAbilityInput → I_GetActionBarItemByEnum(slot).ID → ASC.FindAbilityFromInputID(ID). Filtering the InpActEvt and calling EnqueueAbilityInput(slot, true) + (slot, false) later casts as if the key was pressed then   # verified in game
  ground_target: UBP_GroundTargetAbility_C (HeavenlyStrike, MeteorStrike, ThunderStorm, EmberErruption, Teleport, traps, BlastingShot): GetCastRadius() (CastRadiusData curve, else mCastAreaRadius), GetTargetLocation() = controller I_GetCastSpot() (CastSpot, written by UpdateCastAreaData every tick while a cast area is on). Decal: controller I_CastAreaSwitch(on, mValidCastAreaClass, GetCastRadius, GetAbilityTraceRange) spawns LocalCastAreaRef (e.g. BP_DecalActor_HeavenlyStrike_C) and moves it to the aim; the ability calls it in OnCastStarted / K2_OnEndAbility. Calling it ourselves shows the decal before the cast   # verified in game (Heavenly Strike)
  channel: UBP_GameAbility_ChannelAbility_C subclasses (ChannelHeal_Cone …) must be held: a press + release tap ends them at once
  cleric_bar: Regen / DivineShield / ChannelHeal_Cone / Resurrection TraceFriend; JusticeStrike / Exorcism / DivineSacrifice TraceEnemy + mRequireValidTarget; HolyLight TraceEnemy projectile; HolyBlast / Smite TraceAny projectile (Smite HoldToRepeatAbility); HeavenlyStrike ground target; Beacon Self
  ability_anims: per ability a montage from FindBestMontage() (soft ref, e.g. Mon_HolyBlast_H_M), sections named in mOnActivateAnimInfo / mOnCastAnimInfo / mOnHoldAnimInfo {mSectionName, mCombatModeAsByte}; played with ASC.K2_PlayMontage. Mon_ChannelHeal_H_M: ChannelHeal_Start 0 → _Loop 0.33 (loops) → _End 1.67, slots TP_Upper_R/L + FP_Upper_R/L (upper body only), notifies AnimLockStart @0.33 / AnimLockEnd @1.67 (BP_GameplayAnimNotify → ASC.SendGameplayAnimNotifyToAbilityByTag). The loop clip alone (H_M_TP_Cleric_tSpear_{R,L}_ChannelHeal01_Loop) plays in TP_Upper_{R,L} via PlaySlotAnimationAsDynamicMontage; the AnimBP (UBPA_Base_3rd_C) shows the L chain when MirrorAnims?? @0xE830 is set (MirrorAnimsMontage @0xE98E: role unknown)   # pose verified in game on a mirrored Cleric
  kill_xp: BP_GameplayCombatLibrary DistributeExperience → per group member (≤ 10000 uu) TryGiveExperience → CalculateFinalExp. xp = floor(round(round(Experience[heroLevel].KillExpRewards · EliteExp%/100 · EnemyScalar.ExperienceMultiplier) · clamp(1 + 0.1·(monsterLevel − heroLevel), 0.01, 2.0)) · (tag ItemTag_Exp_10p ? 1.1 : 1)). Base row keyed by HERO level (L1 4, L10 64, L20 123; rows 1..20 only). Bonus caps at ×2 from +10 levels above; no penalty for monsters above you; each member gets full XP (no split). Quests: flat GainExperience. Read: `just data table Archon/Content/DataTables/Stats/Experience`, `just data bp Archon/Content/Blueprints/Ability/BP_GameplayCombatLibrary CalculateFinalExp`   # bytecode verified; not checked in game; native GainExperience (heroism, caps) not decompiled
