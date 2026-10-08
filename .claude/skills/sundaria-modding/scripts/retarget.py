"""Retarget a UE4-mannequin-named animation (Paragon heroes, Epic skeleton names) onto the Sundaria player skeleton.

  blender -b --factory-startup --python retarget.py -- <dos-tool-bones-*.txt> <source anim.fbx> <out.fbx> [helper_model.json]

The target armature is built by skeleton_fbx.build() from the bone dump - the same bones and rest frames as the
skeleton copy in the UE project (verified in game) - so the exported tracks land on the game skeleton as authored.
Method: per mapped bone, the source bone's world rotation change from its own rest pose is applied to the target
bone's rest rotation (both skeletons stand in a similar A-pose, facing -Y in Blender). Unmapped target bones (armor,
cloth, face, twist, IK, weapon bones) keep their rest pose relative to their parent, so they follow - except helper
bones in helper_model.json (helper_model.py, learned from the game's own animations), driven from the main bones. The pelvis
translation moves Bone_Root, scaled by the hip height ratio. Output: one armature named "Armature" with the action.
"""
import json
import os
import sys

import bpy
from mathutils import Matrix, Quaternion, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import retarget_map  # noqa: E402
import skeleton_fbx  # noqa: E402

MAP = retarget_map.MAP
ROOT_SRC, ROOT_DST = "pelvis", "Bone_Root"


def rot(m):
    return m.to_quaternion().normalized()


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    dump, src_fbx, out = argv[0], argv[1], argv[2]
    helper_model = json.load(open(argv[3])) if len(argv) > 3 else {}  # helper_model.py output: skirt/twist/Physique rules
    bones = skeleton_fbx.parse(dump)
    worlds = skeleton_fbx.world_matrices(bones)
    tgt, _ = skeleton_fbx.build(bones, worlds, len(bones), "SK_SundariaProxy")
    sc = bpy.context.scene

    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=src_fbx)
    src = next(o for o in set(bpy.data.objects) - before if o.type == "ARMATURE")
    act = src.animation_data.action
    f0, f1 = int(act.frame_range[0]), int(act.frame_range[1])
    names = [b["name"] for b in bones]
    pairs = [(s, t) for s, t in MAP.items() if s in src.pose.bones and t in tgt.pose.bones]
    missing = [s for s in MAP if s not in src.pose.bones]
    print(f"[retarget] frames {f0}..{f1}, {len(pairs)} bones mapped, missing in source: {missing}")

    # rest poses (frame-independent): source world rest from the bind pose, target armature-space rest
    sw = src.matrix_world
    src_rest = {s: rot(sw @ src.data.bones[s].matrix_local) for s, _ in pairs}
    src_root_rest = (sw @ src.data.bones[ROOT_SRC].matrix_local).to_translation()
    A = rot(tgt.matrix_world)  # target armature -> world rotation
    tw_inv = tgt.matrix_world.inverted()
    rest = {n: tgt.data.bones[n].matrix_local.copy() for n in names}
    dst_root_rest = rest[ROOT_DST].to_translation()
    scale = (tgt.matrix_world @ dst_root_rest).z / max(src_root_rest.z, 1e-3)  # hip height ratio (world)
    by_dst = {t: s for s, t in pairs}

    tgt.animation_data_create()
    tgt.animation_data.action = bpy.data.actions.new("Retarget")
    sc.frame_start, sc.frame_end = f0, f1
    def local_rest(i):
        p = bones[i]["parent"]
        return rest[names[i]] if p < 0 else rest[names[p]].inverted() @ rest[names[i]]

    def solve(helper_basis):
        """Armature-space pose of every bone: mapped bones from the source, helpers from helper_basis, rest follow."""
        pose = {}
        for i, n in enumerate(names):
            p = bones[i]["parent"]
            m = local_rest(i) if p < 0 else pose[names[p]] @ local_rest(i)  # follow the parent, rest offset
            s = by_dst.get(n)
            if s:
                delta = rot(sw @ src.pose.bones[s].matrix) @ src_rest[s].inverted()  # world-space change from rest
                r = A.inverted() @ delta @ A @ rot(rest[n])
                loc = m.to_translation()
                if n == ROOT_DST:
                    moved = (sw @ src.pose.bones[s].matrix).to_translation() - src_root_rest
                    loc = tw_inv @ (tgt.matrix_world @ dst_root_rest + moved * scale)
                m = Matrix.Translation(loc) @ r.to_matrix().to_4x4()
            elif n in helper_basis:
                m = m @ helper_basis[n].to_matrix().to_4x4()
            pose[n] = m
        return pose

    def basis(pose, i):
        p = bones[i]["parent"]
        parent_pose = Matrix.Identity(4) if p < 0 else pose[names[p]]
        return local_rest(i).inverted() @ parent_pose.inverted() @ pose[names[i]]

    def rotvec(q):
        q = q.copy()
        if q.w < 0:
            q.negate()
        axis, ang = q.to_axis_angle()
        return Vector(axis) * ang

    index = {n: i for i, n in enumerate(names)}
    helpers = {h: m for h, m in helper_model.items() if h in index and h not in by_dst}
    print(f"[retarget] {len(helpers)} helper bones driven by the game's own rules")
    for f in range(f0, f1 + 1):
        sc.frame_set(f)
        pose = solve({})
        hb = {}
        for h, m in helpers.items():  # helper delta = bias + W * driver deltas (helper_model.py), Blender frame
            x = [c for d in m["drivers"] for c in rotvec(basis(pose, index[d]).to_quaternion())]
            v = Vector(m["bias"]) + Vector([sum(w * xi for w, xi in zip(row, x)) for row in m["W"]]) if m["W"] else Vector(m["bias"])
            hb[h] = Quaternion(v.normalized(), v.length) if v.length > 1e-9 else Quaternion()
        pose = solve(hb)
        for i, n in enumerate(names):
            pb = tgt.pose.bones[n]
            pb.matrix_basis = basis(pose, i)
            pb.keyframe_insert("location", frame=f)
            pb.keyframe_insert("rotation_quaternion", frame=f)

    for o in list(bpy.data.objects):
        if o not in (tgt,):
            bpy.data.objects.remove(o, do_unlink=True)
    bpy.ops.export_scene.fbx(filepath=out, object_types={"ARMATURE"}, add_leaf_bones=False, primary_bone_axis="Y", secondary_bone_axis="X",
                             apply_unit_scale=True, use_armature_deform_only=False, bake_anim=True, bake_anim_use_all_actions=False,
                             bake_anim_use_nla_strips=False, bake_anim_force_startend_keying=True, bake_anim_simplify_factor=0.0)
    print(f"[retarget] wrote {out}")


main()
