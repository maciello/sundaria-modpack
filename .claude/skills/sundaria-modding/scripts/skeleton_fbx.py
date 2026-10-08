"""Bone dump (dos-tool skeleton probe) -> FBX parts that rebuild the game skeleton in a UE 4.27 project.

  blender -b --factory-startup --python skeleton_fbx.py -- <dos-tool-bones-*.txt> <out dir> [--proxy]
  --proxy: instead, one stick-figure mesh over all bones (SK_SundariaProxy.fbx) to preview animations in the editor

Why parts: animations address skeleton bones by index, so the copy needs the original bone order. A UE FBX
import orders bones depth-first, but the game skeleton was grown by merging meshes (new bones appended). We split
the order into the fewest prefixes whose new bones come out depth-first, write one FBX per prefix, and import them
in order onto one skeleton: UE appends each part's new bones, which reproduces the original order.

Each FBX holds an armature object named "Armature" (UE drops it instead of adding an extra root bone) and a tiny
tetrahedron per bone, weighted 1.0 to it, so every bone is bound. Coordinates: UE (left-handed, cm) -> Blender by
mirroring Y (M @ T @ M); the FBX exporter + UE importer undo that. Scene unit = 1 cm.
"""
import re
import sys

import bpy
from mathutils import Matrix, Quaternion, Vector


def parse(path):
    bones = []
    pat = re.compile(r"\s*(\d+)\s+(-?\d+)\s+(\S+)\s+t (\S+) (\S+) (\S+)\s+q (\S+) (\S+) (\S+) (\S+)\s+s (\S+) (\S+) (\S+)")
    in_first = False
    for line in open(path, encoding="utf-8"):
        if line.startswith("# skeleton"):
            if in_first:
                break  # only the first skeleton in the file
            in_first = True
            continue
        m = pat.match(line)
        if in_first and m:
            g = m.groups()
            assert int(g[0]) == len(bones), "bone indices must be contiguous"
            f = [float(x) for x in g[3:]]
            bones.append({"name": g[2], "parent": int(g[1]), "t": f[0:3], "q": f[3:7], "s": f[7:10]})
    assert bones and bones[0]["parent"] == -1, "no bone table found"
    return bones


def depth_first_new(parent, have, new):
    """Order in which a depth-first walk (children by index) meets the bones of `new` in the tree have|new."""
    kids = {}
    for i in sorted(have | new):
        if parent[i] >= 0:
            kids.setdefault(parent[i], []).append(i)
    out, stack = [], [0]
    while stack:
        i = stack.pop()
        if i in new:
            out.append(i)
        stack.extend(reversed(kids.get(i, [])))
    return out


def parts(parent):
    n, have, ends, i = len(parent), set(), [], 0
    while i < n:
        j = i + 1
        while j < n and depth_first_new(parent, have, set(range(i, j + 1))) == list(range(i, j + 1)):
            j += 1
        ends.append(j)  # this part = bones [0, j)
        have |= set(range(i, j))
        i = j
    return ends


MIRROR = Matrix.Diagonal((1.0, -1.0, 1.0, 1.0))


def world_matrices(bones):
    out = []
    for b in bones:
        x, y, z, w = b["q"]
        local = Matrix.LocRotScale(Vector(b["t"]), Quaternion((w, x, y, z)), Vector(b["s"]))
        out.append(local if b["parent"] < 0 else out[b["parent"]] @ local)
    return [MIRROR @ m @ MIRROR for m in out]  # UE -> Blender


def build(bones, worlds, count, mesh_name, sticks=False):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.unit_settings.system = "METRIC"
    sc.unit_settings.scale_length = 0.01  # 1 Blender unit = 1 cm

    arm = bpy.data.armatures.new("Armature")
    rig = bpy.data.objects.new("Armature", arm)
    sc.collection.objects.link(rig)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")
    eb = []
    for i in range(count):
        b = arm.edit_bones.new(bones[i]["name"])
        b.head, b.tail = (0, 0, 0), (0, 2.0, 0)  # 2 cm long; orientation comes from the matrix
        b.use_connect = False
        if bones[i]["parent"] >= 0:
            b.parent = eb[bones[i]["parent"]]
        m = worlds[i].normalized()
        b.matrix = m
        eb.append(b)
    bpy.ops.object.mode_set(mode="OBJECT")

    verts, faces = [], []
    tet = [Vector((0.5, 0, 0)), Vector((-0.5, 0.5, 0)), Vector((-0.5, -0.5, 0)), Vector((0, 0, 0.7))]
    for i in range(count):
        p = worlds[i].to_translation()
        base = len(verts)
        verts += [p + v for v in tet]
        faces += [(base, base + 1, base + 2), (base, base + 1, base + 3), (base + 1, base + 2, base + 3), (base, base + 2, base + 3)]
    if sticks:
        weights = []
        for i in range(1, count):
            a, b = worlds[bones[i]["parent"]].to_translation(), worlds[i].to_translation()
            if (b - a).length < 0.5:
                continue
            d = (b - a).normalized()
            u = d.orthogonal().normalized() * 0.6
            v = d.cross(u).normalized() * 0.6
            base = len(verts)
            verts += [a + u + v, a + u - v, a - u - v, a - u + v, b + u + v, b + u - v, b - u - v, b - u + v]
            faces += [(base, base + 1, base + 2, base + 3), (base + 4, base + 7, base + 6, base + 5), (base, base + 4, base + 5, base + 1),
                      (base + 1, base + 5, base + 6, base + 2), (base + 2, base + 6, base + 7, base + 3), (base + 3, base + 7, base + 4, base)]
            weights.append((bones[i]["parent"], list(range(base, base + 8))))
    me = bpy.data.meshes.new(mesh_name)
    me.from_pydata([tuple(v) for v in verts], [], faces)
    obj = bpy.data.objects.new(mesh_name, me)
    sc.collection.objects.link(obj)
    for i in range(count):
        vg = obj.vertex_groups.new(name=bones[i]["name"])
        vg.add(list(range(4 * i, 4 * i + 4)), 1.0, "REPLACE")
    if sticks:
        for parent, idx in weights:
            obj.vertex_groups[bones[parent]["name"]].add(idx, 1.0, "REPLACE")
    obj.parent = rig
    mod = obj.modifiers.new("Armature", "ARMATURE")
    mod.object = rig
    return rig, obj


EXPORT = dict(object_types={"ARMATURE", "MESH"}, add_leaf_bones=False, primary_bone_axis="Y", secondary_bone_axis="X", apply_unit_scale=True,
              use_armature_deform_only=False, mesh_smooth_type="FACE")


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    dump, out = argv[0], argv[1].rstrip("/\\")
    if "--proxy" in argv:  # one stick-figure mesh over all bones (editor preview; same skeleton)
        bones = parse(dump)
        build(bones, world_matrices(bones), len(bones), "SK_SundariaProxy", sticks=True)
        bpy.ops.export_scene.fbx(filepath=f"{out}/SK_SundariaProxy.fbx", bake_anim=False, **EXPORT)
        return
    bones = parse(dump)
    parent = [b["parent"] for b in bones]
    worlds = world_matrices(bones)
    ends = parts(parent)
    root_mesh = "H_M_Root_Mesh_04"  # first part's mesh name: UE names the new skeleton <mesh>_Skeleton
    print(f"[skeleton_fbx] {len(bones)} bones, {len(ends)} parts ending at {ends}")
    for k, end in enumerate(ends):
        name = root_mesh if k == 0 else f"SkelPart{k + 1}"
        build(bones, worlds, end, name)
        path = f"{out}/{k + 1}_{name}.fbx"
        bpy.ops.export_scene.fbx(filepath=path, bake_anim=False, **EXPORT)
        print(f"[skeleton_fbx] wrote {path} (bones 0..{end - 1})")


if __name__ == "__main__":
    main()
