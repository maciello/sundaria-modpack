#!/usr/bin/env python3
"""Abilities the DPS tables know, YAML grouped by class. Input: tables file (`dps-tables 1`, format: libs/dps/src/tables_io.cpp:6-12).

  abilities.py [--tables F] [regex]   regex: case-insensitive, matches class or ability name
  abilities.py --self-test
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TABLES = os.environ.get("DPS_TABLES", str(ROOT / "build/dps/tables.txt"))
ELEMS = ["Fire", "Ice", "Void", "Nature", "Lightning", "Holy", "Death", "Light"]  # EMagicDamageType 0..7 (scripts/dps_tables.py)
PRIMARY = {"Champion": "slash", "Ranger": "aimedshot", "Cleric": "smite", "Rogue": "eviscerate", "Wizard": "fireball"}


def parse(text):
    lines = [s.split() for s in text.splitlines() if s.strip()]
    if not lines or lines[0] != ["dps-tables", "1"]:
        sys.exit("!! not a `dps-tables 1` file")
    abilities, cur, mon = [], None, None
    for t in lines:
        k, a = t[0], t[1:]
        if k == "ability":
            cur = {"class": a[0], "name": a[1], "weapons": [], "cooldown": [], "damage": [], "sections": {}}
            abilities.append(cur)
            mon = None
        elif cur is None:
            continue
        elif k == "qualifier":
            cur["weapons"] = [x for x in a if x != "-"]
        elif k == "cooldown":
            cur["cooldown"] = [float(x) for x in a]
        elif k == "montage":
            mon = a[0]
            cur["sections"].setdefault(mon, [])
        elif k == "section":
            cur["sections"][mon].append({"name": a[0], "hits": int(float(a[1])), "length": float(a[4])})
        elif k == "damage":
            cur["damage"].append({
                "src": a[0], "ap_rule": int(a[1]), "magic": a[2] == "1",
                "element": ELEMS[int(a[3])] if a[2] == "1" else None,
                "bonus_stat": "" if a[4] == "-" else a[4], "dot": a[5] == "1",
                "aoe": int(float(a[6])), "aoe_cap": int(float(a[7])),
            })
        elif k == "coef":
            cur["damage"][-1]["coef"] = [float(x) for x in a]
    return abilities


def scalar(v):
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float):
        return f"{v:g}"
    if isinstance(v, int):
        return str(v)
    s = str(v)
    return s if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", s) else json.dumps(s)


def flow(v):
    if isinstance(v, list):
        return "[" + ", ".join(flow(x) for x in v) + "]"
    if isinstance(v, dict):
        return "{" + ", ".join(f"{scalar(k)}: {flow(x)}" for k, x in v.items()) + "}"
    return scalar(v)


def emit(node, ind=0):
    pad, out = "  " * ind, []
    for k, v in node.items():
        if isinstance(v, dict) and v:
            out += [f"{pad}{scalar(k)}:", *emit(v, ind + 1)]
        elif isinstance(v, list) and v and all(isinstance(x, dict) for x in v):
            out += [f"{pad}{scalar(k)}:", *(f"{pad}  - {flow(x)}" for x in v)]
        else:
            out.append(f"{pad}{scalar(k)}: {flow(v)}")
    return out


def missing_primaries(abilities):
    have = {(a["class"], a["name"].lower().replace(" ", "")) for a in abilities}
    return {c: n for c, n in PRIMARY.items() if (c, n) not in have}


def render(abilities, rx=None):
    tree = {}
    for a in abilities:
        if rx and not (rx.search(a["class"]) or rx.search(a["name"])):
            continue
        prim = PRIMARY.get(a["class"]) == a["name"].lower().replace(" ", "")
        tree.setdefault(a["class"], {})[a["name"]] = {
            "primary": prim, "cooldown": a["cooldown"], "weapons": a["weapons"],
            "damage": a["damage"], "sections": a["sections"],
        }
    head = [f"# filter: {rx.pattern} …⊇ (subset)" if rx else "# filter: none (all abilities)"]
    head.append("primary_missing:")
    head += [f"  {c}: {n}" for c, n in missing_primaries(abilities).items()] or ["  {}"]
    return "\n".join(head + emit(tree)) if tree else "\n".join(head + ["# no match"])


def self_test():
    fx = """dps-tables 1
ability Ranger AimedShot 1 Pull
qualifier Crossbow Bow2H
cooldown 0.15 0.15 0.15 0.15 0.15
montage Mon_A
section AimedShot01 1 1 0.7333 1
damage GE_A 1 0 0 Ability_A 0 0 1
coef 1.1 1.2 1.3 1.4 1.5
dotdur 0 0 0 0 0
dotper 0 0 0 0 0
ability Rogue Eviscerate 0 SL
cooldown 1 1 1 1 1
damage GE_E 0 0 1 X 0 0 1
coef 2 2 2 2 2
"""
    ab = parse(fx)
    assert [(a["class"], a["name"]) for a in ab] == [("Ranger", "AimedShot"), ("Rogue", "Eviscerate")]
    assert ab[0]["damage"][0]["coef"][0] == 1.1 and ab[0]["damage"][0]["element"] is None
    assert ab[0]["sections"]["Mon_A"][0]["hits"] == 1 and ab[1]["damage"][0]["element"] is None
    out = render(ab)
    assert "Eviscerate:\n    primary: true" in out and "AimedShot:\n    primary: true" in out
    assert "Champion: slash" in out
    assert render(ab, re.compile("salvo", re.I)).endswith("# no match")
    print("abilities self-test ok")


def main():
    if "--self-test" in sys.argv:
        return self_test()
    p = argparse.ArgumentParser()
    p.add_argument("--tables", default=DEFAULT_TABLES)
    p.add_argument("regex", nargs="?")
    args = p.parse_args()
    f = Path(args.tables)
    if not f.is_file():
        sys.exit(f"!! no tables file {f} (just dps-tables <model.json>)")
    rx = re.compile(args.regex, re.I) if args.regex else None
    print(render(parse(f.read_text()), rx))


if __name__ == "__main__":
    main()
