"""Paragon / UE4 mannequin bone names -> Sundaria player skeleton main bones (shared by retarget.py, helper_model.py)."""

# source (Epic names) -> target (Sundaria). Bone_Root parents both Bone_hips (legs) and Bone_Spine, like pelvis.
MAP = {"pelvis": "Bone_Root", "spine_01": "Bone_Spine", "spine_02": "Bone_Spine1", "spine_03": "Bone_Spine2",
       "neck_01": "Bone_Neck", "head": "Bone_Head"}
for s, t in (("l", "Left"), ("r", "Right")):
    MAP.update({f"clavicle_{s}": f"Bone_{t}Shoulder", f"upperarm_{s}": f"Bone_{t}Arm", f"lowerarm_{s}": f"Bone_{t}ForeArm",
                f"hand_{s}": f"Bone_{t}Hand", f"thigh_{s}": f"Bone_{t}UpLeg", f"calf_{s}": f"Bone_{t}Leg",
                f"foot_{s}": f"Bone_{t}Foot", f"ball_{s}": f"Bone_{t}ToeBase"})
    for f, g in (("thumb", "Thumb"), ("index", "Index"), ("middle", "Middle"), ("ring", "Ring"), ("pinky", "Pinky")):
        for k in (1, 2, 3):
            MAP[f"{f}_0{k}_{s}"] = f"Bone_{t}Hand{g}{k}"
