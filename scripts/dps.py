#!/usr/bin/env python3
"""Offline DPS host (#118): the shared DPS library (libs/dps, build/libdps.so) without the game running. YAML out.

  dps.py hero <snapshot.yaml>                    DPS + per-ability breakdown of a saved hero (char-snapshot yaml)
  dps.py score [--items F ...]                   every item of the given files (default: all heroes' equipped gear)
                                                 against each saved hero: DPS change if it replaced their item
  dps.py bis <Class> [--level L] [--items F ...] best item per slot for a class from the given items
  dps.py weights <snapshot.yaml|Class> [--grade G]   % DPS per affix, best first
  dps.py calibrate <snapshot.yaml>               predict every stat of the hero's items from (spec, grade, level)
  dps.py bench [snapshot.yaml]                   kernel 10k builds + scoring all items, best of N

Common: --scenario NAME (built-in synthetic targets, or --scenarios FILE yaml {scenarios: {name: {...}}}).
Env: DPS_TABLES (tables file from scripts/dps_tables.py), DPS_CHARS (dos-tool-chars folder), DPS_LIB (libdps.so).
Item files: char-snapshot yaml (`equipped:`) or any yaml with an `items:` list of the same item records."""
import argparse
import ctypes as ct
import os
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    sys.exit("needs PyYAML (pip install pyyaml)")

ROOT = Path(__file__).resolve().parent.parent
# synthetic targets (placeholders for contrast, not game data)
SCENARIOS = {
    "boss": {"targets": 1, "fight_s": 120, "target_level_delta": 2, "armor": 400, "magic_resist": 400},
    "armoured": {"targets": 1, "fight_s": 120, "target_level_delta": 2, "armor": 2000, "magic_resist": 400},
    "resist": {"targets": 1, "fight_s": 120, "target_level_delta": 2, "armor": 400, "magic_resist": 400,
               "resist": {"Fire": 0.5, "Nature": 0.5, "Void": 0.5, "Holy": 0.5}},
    "aoe5": {"targets": 5, "fight_s": 30, "target_level_delta": 0, "armor": 300, "magic_resist": 300},
    "burst15": {"targets": 1, "fight_s": 15, "target_level_delta": 2, "armor": 400, "magic_resist": 400},
}


def tok(s):
    return "-" if s in (None, "") else str(s).replace(" ", "_")


class Lib:
    def __init__(self, lib, tables):
        self.dll = ct.CDLL(str(lib))
        self.dll.dps_open.restype = ct.c_void_p
        self.dll.dps_open.argtypes = [ct.c_char_p, ct.c_char_p, ct.c_int]
        self.dll.dps_call.argtypes = [ct.c_void_p, ct.c_char_p, ct.c_char_p, ct.c_int]
        self.dll.dps_item_stat.restype = ct.c_float
        self.dll.dps_item_stat.argtypes = [ct.c_void_p, ct.c_int, ct.c_char_p, ct.c_int, ct.c_int]
        err = ct.create_string_buffer(512)
        self.h = self.dll.dps_open(str(tables).encode(), err, 512)
        if not self.h:
            sys.exit(f"!! {err.value.decode()} (DPS_TABLES; make one: just dps-tables <model.json>)")

    def call(self, lines):
        req = "\n".join(lines).encode()
        cap = 1 << 16
        while True:
            buf = ct.create_string_buffer(cap)
            n = self.dll.dps_call(self.h, req, buf, cap)
            if n < cap:
                return yaml.safe_load(buf.value.decode())
            cap = n + 1


def scenario_lines(name, extra):
    sc = (SCENARIOS | extra).get(name) or sys.exit(f"!! unknown scenario {name}: {sorted(SCENARIOS | extra)}")
    out = [f"scenario {name} {sc['targets']} {sc['fight_s']} {sc['target_level_delta']} {sc['armor']} {sc['magic_resist']} "
           f"{sc.get('glancing', 0)} {sc.get('deflect', 0)} {sc.get('incoming_mod', 0)} {int(sc.get('rotation') == 'event')}"]
    return out + [f"resist {k} {v}" for k, v in (sc.get("resist") or {}).items()]


def item_lines(kind, it):
    out = [f"{kind} {it['spec']} {tok(it.get('equip_slot'))} {tok(it.get('type'))} {it['grade']} {it['level']} {it.get('slot', -1)} "
           f"{it.get('name', '')}"]
    return out + [f"stat {k} {v}" for k, v in (it.get("stats") or {}).items()]


def hero_lines(snap, with_gear=True):
    out = [f"build {snap['class']} {snap['level']} 3"]
    if snap.get("primary_stats"):
        out.append("primary " + " ".join(map(str, snap["primary_stats"])))
    out += [f"learned {a['name']} {a['level']}" for a in snap.get("learned_abilities") or []]
    if with_gear:
        for it in snap.get("equipped") or []:
            out += item_lines("equipped", it)
    return out


def load(p):
    return yaml.safe_load(Path(p).read_text())


def snapshots(chars):
    return sorted(Path(chars).glob("*.yaml"))


def items_of(files):
    out = []
    for f in files:
        d = load(f)
        for it in d.get("items") or d.get("equipped") or []:
            out.append(dict(it, source=f"{Path(f).name}:{d.get('name', '')}"))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["hero", "score", "bis", "weights", "calibrate", "bench"])
    ap.add_argument("target", nargs="?")
    ap.add_argument("--items", nargs="*", default=None)
    ap.add_argument("--scenario", default="boss")
    ap.add_argument("--scenarios", type=Path)
    ap.add_argument("--level", type=int, default=20)
    ap.add_argument("--grade", type=int, default=3, help="EItemGrade value (3 Rare)")
    ap.add_argument("--reps", type=int, default=20)
    ap.add_argument("--min-pct", type=float, default=0.0, help="score: list items above this DPS change")
    a = ap.parse_args()
    tables = os.environ.get("DPS_TABLES") or sys.exit("!! set DPS_TABLES (just dps sets it)")
    lib = Lib(os.environ.get("DPS_LIB", ROOT / "build/libdps.so"), tables)
    chars = os.environ.get("DPS_CHARS", "")
    extra = load(a.scenarios)["scenarios"] if a.scenarios else {}
    sc = scenario_lines(a.scenario, extra)
    files = a.items if a.items is not None else [str(p) for p in snapshots(chars)]

    if a.cmd == "hero":
        print(yaml.safe_dump(lib.call(["cmd dps", *sc, *hero_lines(load(a.target))]), sort_keys=False, width=160))
    elif a.cmd == "score":
        cands = items_of(files)
        out = {"scenario": a.scenario, "items": len(cands), "heroes": []}
        for p in snapshots(chars):
            snap = load(p)
            req = ["cmd score", *sc, *hero_lines(snap)] + [x for it in cands for x in item_lines("cand", it)]
            r = lib.call(req)
            ups = [dict(i, source=cands[i["i"]]["source"]) for i in r["items"] if i["fits"] and i["delta_pct"] > a.min_pct]
            ups.sort(key=lambda i: -i["delta_pct"])
            out["heroes"].append({"hero": f"{snap['name']} ({snap['class']} L{snap['level']})", "dps": r["dps"],
                                  **({"error": r["error"]} if r.get("error") else {}), "upgrades": ups})
        print(yaml.safe_dump(out, sort_keys=False, width=200))
    elif a.cmd == "bis":
        cands = items_of(files)
        req = ["cmd bis", *sc, f"build {a.target} {a.level} 3"] + [x for it in cands for x in item_lines("cand", it)]
        r = lib.call(req)
        for p in r.get("picks") or []:
            if p["i"] >= 0:
                p["source"] = cands[p["i"]]["source"]
        print(yaml.safe_dump({"class": a.target, "level": a.level, "scenario": a.scenario, "candidates": len(cands), **r},
                             sort_keys=False, width=200))
    elif a.cmd == "weights":
        hero = hero_lines(load(a.target)) if a.target and a.target.endswith(".yaml") else [f"build {a.target} {a.level} 3"]
        print(yaml.safe_dump({"scenario": a.scenario, "grade": a.grade, **lib.call(["cmd weights", *sc, *hero, f"grade {a.grade}"])},
                             sort_keys=False, width=200))
    elif a.cmd == "calibrate":
        r = lib.call(["cmd predict", *hero_lines(load(a.target))])
        total = exact = 0
        for it in r["items"]:
            for st, v in (it.get("stats") or {}).items():
                total += 1
                ok = v["pred"] is not None and abs(v["pred"] - v["actual"]) < 1e-6
                exact += ok
                if not ok:
                    v["miss"] = True
        print(yaml.safe_dump({"snapshot": str(a.target), "stats": total, "exact": exact, "missed": total - exact, **r},
                             sort_keys=False, width=200))
    elif a.cmd == "bench":
        snap = load(a.target or snapshots(chars)[0])
        cands = items_of(files)
        cands = (cands * (300 // max(len(cands), 1) + 1))[:300]   # a full bag + bank
        req = ["cmd bench", *sc, *hero_lines(snap), f"reps {a.reps}"] + [x for it in cands for x in item_lines("cand", it)]
        print(yaml.safe_dump(lib.call(req), sort_keys=False))


if __name__ == "__main__":
    main()
