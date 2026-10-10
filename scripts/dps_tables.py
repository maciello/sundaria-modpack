#!/usr/bin/env python3
"""Normalised pak extract (model.json) -> tables file the DPS library reads offline (libs/dps FileSource).

Usage: dps_tables.py <model.json> <tables.txt>. Both stay outside the repo (game data). Record format:
libs/dps/src/tables_io.cpp. Ability rules that resolve pak data per ability level 1..5 live here: Ability curve
value, DoT duration/period rows next to the damage row, AoE source, cooldown row fallback, conditional components."""
import json
import re
import sys
from pathlib import Path

CLASSES = ["Champion", "Cleric", "Wizard", "Rogue", "Ranger"]
ELEMS = ["Fire", "Ice", "Void", "Nature", "Lightning", "Holy", "Death", "Light"]  # EMagicDamageType 0..7
CLASS_AP = {"Champion": 0, "Cleric": 2, "Wizard": 2, "Rogue": 0, "Ranger": 1}  # AttackPowerScaleRule 4 (class tag)
LEARNED_AS = {"RapidShots": ["RapidFire"], "ParalysingShot": ["ParalyzingShot"]}  # ability folder -> char snapshot names
LV = range(1, 6)


def tok(x):
    s = "-" if x is None or x == "" else str(x)
    if re.search(r"\s", s):
        sys.exit(f"!! token with whitespace: {s!r}")
    return s


def num(x):
    return f"{float(x):.9g}"


class Abilities:
    def __init__(self, m, cls):
        self.curves = m["abilities"][cls]["curves"]
        self.cls = cls

    def curve(self, ref, lv):
        t, r = ref
        v = self.curves.get(t, {}).get(r)
        return None if v is None else float(v[min(max(lv, 1), len(v)) - 1])

    def dot_rows(self, table, row, lv):
        """<prefix>.Duration / <prefix>.Period next to the damage row (inferred)."""
        rows = self.curves.get(table, {})
        pre, first = row.rsplit(".", 1)[0].lower(), row.split(".")[0].lower()

        def find(word):
            for p in (pre, first):
                for k in rows:
                    if k.lower() == f"{p}.{word}":
                        return self.curve((table, k), lv)
            return None
        return find("duration"), find("period")

    def aoe(self, a, d, cref):
        """0 single target, 1 every target, 2 min(targets, projectile count)."""
        rows = self.curves.get(cref[0], {})
        pre = cref[1].split(".")[0].lower()
        if (a["projectile_damage_radius"] or {}).get("CurveTable") and d["src"] in a["on_projectile_hit_ges"]:
            return 1
        if a.get("cone_degree"):
            return 1
        if a["multishot_rotations"]:
            return 2
        for k in rows:
            kl = k.lower()
            if "radius" in kl and not any(x in kl for x in ("trigger", "detect", "meters")) and \
                    (kl == "radius" or kl.startswith(pre + ".") or "explosion" in kl):
                return 1
        return 0

    def lines(self, out, name, a):
        comps = []
        for d in a["damage"]:
            c = d["coef"]
            if d["conditional"] or not c or not c.get("curve") or c["curve"][0] not in self.curves or c["value"] <= 0:
                continue
            coef = [c["value"] * (self.curve(c["curve"], lv) or 0.0) for lv in LV]
            ap = {1: 0, 2: 1, 3: 2, 4: CLASS_AP[self.cls]}.get(d["ap_rule"], {"Melee": 0, "Range": 1, "Magic": 2}[d["attack_type"]])
            dp = [self.dot_rows(c["curve"][0], c["curve"][1], lv) for lv in LV]
            is_dot = bool(d["dot_duration"] or d["dot_period"] or any(du and pe and pe < du for du, pe in dp))
            comps.append((d, coef, ap, is_dot, dp, self.aoe(a, d, c["curve"])))
        if not comps:
            return
        cd = []
        for lv in LV:
            v = self.curve(a["cooldown_curve"], lv) if a["cooldown_curve"] and a["cooldown_curve"][0] in self.curves else None
            t = comps[0][0]["coef"]["curve"][0]
            for k in ("CoolDown", "Cooldown"):
                v = v if v is not None else self.curve((t, k), lv)
            cd.append(v or 0.0)
        rate = [(self.curve(a["anim_rate_curve"], lv) if a["anim_rate_curve"] else None) or 1.0 for lv in LV]
        proj = "1" if a["on_projectile_hit_ges"] and not a["on_anim_notify_ges"] else "0"
        out += [f"ability {tok(self.cls)} {tok(name)} {proj} {tok(a['activate_section'])}",
                " ".join(["learned", *map(tok, LEARNED_AS.get(name, []))]),
                " ".join(["qualifier", *map(tok, a["weapon_qualifier"])])]
        out += [f"playrate {tok(w)} {num(r)}" for w, r in a["weapon_play_rate"].items()]
        out += [" ".join(["animrate", *map(num, rate)]), " ".join(["cooldown", *map(num, cd)])]
        for mname, mon in a["montages"].items():
            out.append(f"montage {tok(mname)}")
            out += [f"section {tok(s)} {num(v['hits'])} {num(v.get('shots') or 0)} {num(v['lock_end'] or 0)} {num(v['length'])}"
                    for s, v in mon["sections"].items()]
        for d, coef, ap, is_dot, dp, aoe in comps:
            out += [f"damage {tok(d['src'].split('/')[-1])} {ap} {int(d['attack_type'] == 'Magic')} {ELEMS.index(d['magic_type'])} "
                    f"{tok(d['ability_attr'])} {int(is_dot)} {aoe} {a['multishot_rotations'] or 1}",
                    " ".join(["coef", *map(num, coef)]),
                    " ".join(["dotdur", *(num((du or 0) if is_dot else 0) for du, _ in dp)]),
                    " ".join(["dotper", *(num((pe or 0) if is_dot else 0) for _, pe in dp)])]


def tables(m):
    en = m["enums"]
    out = ["dps-tables 1", " ".join(["stats", *map(tok, en["EStatType"])]), " ".join(["percent", *map(tok, m["pct_stats"]["v"])])]
    out += [" ".join(["master", tok(s), *map(num, v)]) for s, v in m["master_stat"]["by_level"].items()]
    out += [f"slot {tok(s)} {num(v)}" for s, v in m["slot_distribution"]["v"].items()]
    out.append(" ".join(["ignored", *map(tok, m["item_creation"]["v"].get("StatTypeIgnoredDistribution", []))]))
    out += [" ".join(["equiv", tok(r), *(num(v.get(k, 1.0)) for k in ("MAP", "RAP", "SP", "BaseToPlate", "ResistElementalBaseToCloth"))])
            for r, v in m["armor_equivalency"]["v"].items()]
    out.append(" ".join(["grades", *map(tok, en["EItemGrade"])]))
    keys = ("AdditionalAttributesMin", "AdditionalAttributesMax", "FillerMin", "FillerMax", "MandatoryByGradeMin", "MandatoryByGradeMax")
    out += [" ".join(["grade", tok(g), num(v["Quality"]), num(v["ElementalMagicMod"]), *(str(int(v[k])) for k in keys)])
            for g, v in m["ITEMGRADE_Table"]["v"].items()]
    for k, e in (("armortypes", "EArmorType"), ("equipslots", "BP_ItemEquipmentSlotEnum"), ("classes", "EClassname")):
        out.append(" ".join([k, *map(tok, en[e])]))
    out += [f"weapon {tok(r)} {tok(v['AnimationType'])} {num(v['DamageModifier'])}" for r, v in m["Weapon_Stats"]["v"].items()]
    out += [f"wdtype {tok(r)} {tok(v['damage_type'])}" for r, v in m["weapon_items"]["v"].items()]
    out += [f"ap {tok(r)} {tok(v['1stStat'])} {num(v['1stMod'])} {tok(v['2ndStat'])} {num(v['2ndMod'])} {num(v['LevelMod'])}"
            for r, v in m["AttackPowerTable"]["v"].items()]
    pd = m["primary_default"]["v"]
    out.append(" ".join(["primary", *(num(pd[k]) for k in ("Strength", "Dexterity", "Intelligence", "Wisdom", "Constitution", "Charisma"))]))
    out.append(f"maxlevel {m['max_level']['v']}")
    grades = [g for g in en["EItemGrade"] if not g.endswith("_MAX")]
    for tag, r in m["crafting_bonus"]["v"].items():
        out.append(" ".join(["pool", tok(tag), "mandatory", *map(tok, r.get("Mandatory1", []))]))
        for g in grades:
            for i, key in enumerate((g, f"Filler_{g}", f"Mandatory_Bonus_{g}")):
                if r.get(key):
                    out.append(" ".join(["pool", tok(tag), tok(g), str(i), *map(tok, r[key])]))
    slots, ats = en["BP_ItemEquipmentSlotEnum"], en["EArmorType"]
    for sid, sp in m["item_specs"]["v"].items():
        out.append(" ".join(["spec", sid, tok(None if sp["slot"] is None else slots[sp["slot"]]),
                             tok(None if sp["armor_type"] is None else ats[sp["armor_type"]]), tok(sp["weapon_type"]),
                             tok(sp["tag"]), str(sp["random_stat_type"] or 0), str(-1 if sp["job"] is None else sp["job"])]))
    hp, hc = m["HeroismPassive"]["v"], m["HeroismCurve"]["v"]
    out += [f"heroism {tok(n)} {int(hp[n]['MaxPoints'])} {num(hc[n])}" for n in hc if n in hp]
    # default attack of ranged weapons = ShootArrow (BP_PlayerControllerGame::AssignDefaultAbilityOnEquip: tag Ability.Ranger.ShootArrow
    # for Crossbow / Bow2H / Crossbow2H, else Ability.MeleeAttack); its WeaponTypesQualifier names exactly those types
    for cls in CLASSES:
        sa = m["abilities"][cls]["abilities"].get("ShootArrow")
        if sa:
            out.append(" ".join(["rangedattack", "ShootArrow", *map(tok, sa["weapon_qualifier"])]))
            break
    for cls in CLASSES:
        ab = Abilities(m, cls)
        for name, a in m["abilities"][cls]["abilities"].items():
            ab.lines(out, name, a)
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    out = tables(json.loads(Path(sys.argv[1]).read_text()))
    Path(sys.argv[2]).write_text("\n".join(out) + "\n")
    print(f"{sys.argv[2]}: {len(out)} records")


if __name__ == "__main__":
    main()
