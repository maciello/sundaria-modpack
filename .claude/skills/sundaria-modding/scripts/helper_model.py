"""Learn how the game's own animations move helper bones (skirt, Physique, twist ...) from the main bones.

  blender -b --factory-startup --python helper_model.py -- <dos-tool-bones-*.txt> <dos-tool-pose-*.txt> <out.json>
  (Blender only for its bundled numpy; no scene is used)

Input: the bone dump (rest pose) and a pose recording (pose recorder: local rotations of every mesh bone while the
game animates). Each bone's rotation is taken as a delta from its rest rotation (q_rest^-1 * q), as a rotation vector,
converted to Blender's mirrored frame (x, y, z) -> (-x, y, -z) so retarget.py can apply it to matrix_basis directly.
For every helper bone that really moves, a ridge regression picks up to 3 main bones (greedy, by held-out R^2) and fits
  helper_delta = bias + W * [driver deltas]
Bones whose motion is not explained (R^2 < 0.5: physics, keyed by hand) are reported and left out.
"""
import json
import re
import sys

import numpy as np

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import retarget_map  # noqa: E402  (MAP: Paragon -> Sundaria main bones)

MAIN = set(retarget_map.MAP.values())


def qmul(a, b):  # (x, y, z, w) arrays [..., 4]
    ax, ay, az, aw = np.moveaxis(a, -1, 0)
    bx, by, bz, bw = np.moveaxis(b, -1, 0)
    return np.stack([aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz], -1)


def rotvec(q):
    q = q * np.sign(q[..., 3:4] + 1e-12)  # w >= 0: shortest arc
    s = np.linalg.norm(q[..., :3], axis=-1, keepdims=True)
    ang = 2 * np.arctan2(s, q[..., 3:4])
    return np.where(s > 1e-9, q[..., :3] / np.maximum(s, 1e-9) * ang, 2 * q[..., :3])


SKIP = ("Face", "Eye", "Jaw", "Tongue", "Node_", "Hair", "Cape", "Beard", "Ear", "Weapon", "Shield", "Projectile")  # own motion/physics
MAX_DIST = 3  # drivers must be within 3 bones of the helper in the tree (stops spurious leg -> face fits)


def tree_dist(parent, a, b):
    up = {}
    d, n = 0, a
    while n:
        up[n] = d
        n, d = parent.get(n), d + 1
    d, n = 0, b
    while n:
        if n in up:
            return up[n] + d
        n, d = parent.get(n), d + 1
    return 99


def load(dump, rec):
    rest, parent, names_by_idx = {}, {}, []
    tree = re.compile(r"\s*(\d+)\s+(-?\d+)\s+(\S+)\s+t ")
    for line in open(dump, encoding="utf-8"):
        m = tree.match(line)
        if m:
            names_by_idx.append(m[3])
            if int(m[2]) >= 0:
                parent[m[3]] = names_by_idx[int(m[2])]
    pat = re.compile(r"\s*\d+\s+-?\d+\s+(\S+)\s+t \S+ \S+ \S+\s+q (\S+) (\S+) (\S+) (\S+)")
    for line in open(dump, encoding="utf-8"):
        m = pat.match(line)
        if m:
            rest[m[1]] = np.array([float(x) for x in m.groups()[1:]])
    lines = [l for l in open(rec, encoding="utf-8") if not l.startswith("#")]
    names = lines[0].split()[1:]
    data = np.array([[float(x) for x in l.split()] for l in lines[1:]]).reshape(len(lines) - 1, len(names), 7)
    deltas = {}
    for i, n in enumerate(names):
        if n not in rest:
            continue
        r = rest[n] * np.array([-1, -1, -1, 1])  # inverse of the unit rest quat
        rv = rotvec(qmul(np.broadcast_to(r, data[:, i, :4].shape), data[:, i, :4]))
        deltas[n] = rv * np.array([-1, 1, -1])  # UE frame -> Blender's mirrored frame
    return deltas, parent


def fit(y, xs, lam=1e-3):
    X = np.hstack([np.ones((len(y), 1))] + xs)
    n = int(len(y) * 0.7)  # train on the first 70 %, score on the rest
    reg = lam * np.eye(X.shape[1])
    reg[0, 0] = 0
    w = np.linalg.solve(X[:n].T @ X[:n] + reg, X[:n].T @ y[:n])
    res = y[n:] - X[n:] @ w
    r2 = 1 - (res ** 2).sum() / max(((y[n:] - y[n:].mean(0)) ** 2).sum(), 1e-12)
    w = np.linalg.solve(X.T @ X + reg, X.T @ y)  # final fit on everything
    return w, r2


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    deltas, parent = load(argv[0], argv[1])
    mains = [n for n in deltas if n in MAIN]
    model, report = {}, []
    for h, y in deltas.items():
        if h in MAIN or any(k in h for k in SKIP):
            continue
        motion = np.degrees(np.linalg.norm(y - y.mean(0), axis=1)).max()
        if motion < 2.0:  # never moves more than 2 degrees: rest (or a constant offset) is fine
            if np.degrees(np.linalg.norm(y.mean(0))) > 1.0:
                model[h] = {"drivers": [], "bias": y.mean(0).tolist(), "W": [], "r2": 1.0}
            continue
        chosen, best_r2 = [], -1e9
        for _ in range(3):
            cand = []
            for d in mains:
                if d in chosen or tree_dist(parent, h, d) > MAX_DIST:
                    continue
                _, r2 = fit(y, [deltas[c] for c in chosen + [d]])
                cand.append((r2, d))
            if not cand:
                break
            r2, d = max(cand)
            if r2 < best_r2 + 0.02:
                break
            chosen.append(d)
            best_r2 = r2
        w, r2 = fit(y, [deltas[c] for c in chosen])
        report.append((h, motion, r2, chosen))
        if r2 >= 0.5:
            model[h] = {"drivers": chosen, "bias": w[0].tolist(), "W": w[1:].T.tolist(), "r2": float(r2)}
    for h, motion, r2, chosen in sorted(report, key=lambda r: -r[1]):
        print(f"[helper_model] {h:32s} moves {motion:5.1f} deg  R2 {r2:5.2f}  {'OK ' if r2 >= 0.5 else 'skip'} <- {', '.join(chosen)}")
    json.dump(model, open(argv[2], "w"), indent=1)
    print(f"[helper_model] {len(model)} helper bones modelled -> {argv[2]}")


main()
