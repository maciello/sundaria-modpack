# Custom animations on the player skeleton (Paragon → Sundaria)

Goal: play new animations (Paragon heroes from Fab) on the player character, keeping its own armor and weapons.
Proven 2026-10-08: skeleton copy verified bone-for-bone in game, retargeted Greystone animations play in the FullBody slot.

## Facts this rests on (verified in game)
- Every playable race/sex uses ONE skeleton: `/Game/PlayerCharacters/Human/Male/_Base/Meshes/H_M_Root_Mesh_04_Skeleton`,
  184 bones (checked 7 lobby characters + 2 players, identical tables). Body, head, hair and all 12 armor slots
  (`ABP_BipedCharacter_C::MeshComp*`) use it and master-pose to `CharacterMesh0`, so armor follows whatever the body plays.
- Bone table: `USkeleton` + 0x58 = `FReferenceSkeleton` (RawRefBoneInfo, FMeshBoneInfo stride 12 = FName + parent; then RawRefBonePose, FTransform 0x30).
- Montage slots: the skeleton's `SlotGroups`; `DefaultGroup.FullBody` accepts `PlaySlotAnimationAsDynamicMontage`.
- The Windows pak is not signed: an unsigned `Content/Paks/~mods/<name>_P.pak` built with UE 4.27 UnrealPak loads.
- Clothes hang on helper bones (`Bone_Armor_Skirt_*`, `Bone_Physique_*`, `*Twist*`) that the game's animations key
  as functions of the main bones (R² 0.8–1.0); cape and hair do not follow (physics or own motion).

## Pipeline (local UE 4.27 project, outside the repo: Fab assets must never be committed)
| step | tool | result |
|---|---|---|
| 1. dump bones | dos-tool "Skeleton probe" → Dump | `dos-tool-bones-<class>.txt` next to the game exe |
| 2. FBX parts | `scripts/skeleton_fbx.py` (Blender, headless) | 5 FBX: the original bone order splits into 5 depth-first prefixes; importing them in order onto one skeleton reproduces the order (animations address bones by index) |
| 3. skeleton copy | UE Python: import parts onto one skeleton, at the game's path AND at `/Game/SundariaMod/SkelVerify` | copy at the game path is what animations reference; it is cooked as a dependency but NEVER packed (it would replace the game's skeleton) |
| 4. verify | pak only the SkelVerify copy, dos-tool "Skeleton probe" → Verify | `OK: 184 bones, same order; drift 0.002 cm` |
| 5. learn helpers | dos-tool "Pose recorder" (game's own anims) → `scripts/helper_model.py` | ridge-regression rules helper = f(nearby main bones) |
| 6. retarget | export Paragon anim FBX (UE Python), `scripts/retarget.py <dump> <src.fbx> <out.fbx> <helper_model.json>` | per main bone: source world rotation change from rest applied to target rest (map: `scripts/retarget_map.py`); helpers from step 5 |
| 7. import, cook, pak | UE Python import onto the game-path skeleton → `/Game/SundariaMod/Anims`; cook with `DirectoriesToAlwaysCook`; UnrealPak with an explicit list of `SundariaMod/Anims` only | `~mods/SundariaAnims_P.pak` |
| 8. play | dos-tool "Anim test" | dynamic montage on the controlled character |

## Not possible / open
- Paragon *models* on the player: re-rig them onto this skeleton (Blender) or the game's montages (hit notifies!) stop working.
- Replacing a game animation = pak an asset at the game's own path; needs the path (anim probe, not built).
- Co-op: friends need the same pak (ship it in the release zip).
