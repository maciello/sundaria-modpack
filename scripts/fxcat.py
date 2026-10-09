"""`just data fx [<path regex>] [--all]`: game particle templates (Cascade + Niagara) summarised for core/fx.hpp.
One YAML row per system: emitters, loop or burst, lifetime, spawn rate, spawn volume, sprite size, dynamic light,
instance parameters (fx::Color/Float/Vector) and material vector parameters (fx::MaterialColor). Default: only the
usable ones for a world marker (small, continuous, no mesh/beam emitters); --all lists every system.
Skill ref: references/fx.md."""
import re


def _idx(ref):
    p = (ref or {}).get("ObjectPath", "") if isinstance(ref, dict) else ""
    tail = p.rsplit(".", 1)[-1]
    return int(tail) if tail.isdigit() else None


def _pkg(ref):  # external object reference -> package path
    p = (ref or {}).get("ObjectPath", "") if isinstance(ref, dict) else ""
    return p.rsplit(".", 1)[0] if p else None


def _vals(v):
    return ((v or {}).get("Table") or {}).get("Values", []) if isinstance(v, dict) else []


def _rng(v):
    t = _vals(v)
    return [round(min(t), 2), round(max(t), 2)] if t else None


def cascade(exports):
    """ParticleSystem exports -> summary dict (materials as package paths, resolved later)."""
    sysx = next((e for e in exports if e.get("Type") == "ParticleSystem"), None)
    if not sysx:
        return None
    by = dict(enumerate(exports))
    ems, params = [], set()
    for e in exports:
        if e.get("Type", "").startswith("DistributionFloatParticleParameter"):
            params.add((e["Properties"].get("ParameterName", "?"), "float"))
        elif e.get("Type", "").startswith("DistributionVectorParticleParameter"):
            params.add((e["Properties"].get("ParameterName", "?"), "vector/color"))
    for er in sysx.get("Properties", {}).get("Emitters", []):
        em = by.get(_idx(er)) or {}
        lods = em.get("Properties", {}).get("LODLevels", [])
        lod = by.get(_idx(lods[0]), {}).get("Properties", {}) if lods else {}
        if not lod or lod.get("bEnabled") is False:
            continue
        req = by.get(_idx(lod.get("RequiredModule")), {}).get("Properties", {})
        sp = by.get(_idx(lod.get("SpawnModule")), {}).get("Properties", {})
        info = {"mat": _pkg(req.get("Material")), "loops": req.get("EmitterLoops", 0), "rate": _rng(sp.get("Rate")),
                "rate_scale": _rng(sp.get("RateScale")), "burst": [b.get("Count") for b in sp.get("BurstList", []) if b.get("Count")]}
        for m in (by.get(_idx(r), {}) for r in lod.get("Modules", [])):
            t, p = m.get("Type", ""), m.get("Properties", {})
            if t == "ParticleModuleLifetime":
                info["life"] = _rng(p.get("Lifetime"))
            elif t.startswith("ParticleModuleLocationPrimitive") or t == "ParticleModuleLocation":
                ext = [abs(x) for x in _vals(p.get("StartRadius")) + _vals(p.get("StartHeight")) + _vals(p.get("StartLocation"))]
                info["volume"] = round(max(ext), 1) if ext else 0
            elif t == "ParticleModuleSize":
                info["size"] = _rng(p.get("StartSize"))
            elif t.startswith("ParticleModuleTypeData"):
                info["type"] = t[len("ParticleModuleTypeData"):]
            elif t == "ParticleModuleLight":
                info["light"] = True
        ems.append(info)
    return {"kind": "cascade", "emitters": ems, "params": sorted(params)}


def niagara(exports):
    sysx = next((e for e in exports if e.get("Type") == "NiagaraSystem"), None)
    if not sysx:
        return None
    text = repr(exports)
    users = sorted(set(re.findall(r"User\.[A-Za-z0-9_ ]+", text)))
    emitters = [e for e in exports if e.get("Type") == "NiagaraEmitter"]
    return {"kind": "niagara", "emitters": [{"name": e.get("Name")} for e in emitters], "params": [(u, "?") for u in users]}


def vector_params(mat_exports):
    """Material / MI exports -> (vector parameter names, parent package or None)."""
    names, parent = set(), None
    for e in mat_exports:
        p = e.get("Properties", {})
        parent = parent or _pkg(p.get("Parent"))
        for v in p.get("VectorParameterValues", []):
            names.add(v.get("ParameterInfo", {}).get("Name"))
        ced = p.get("CachedExpressionData") or e.get("CachedExpressionData") or {}
        for i in (ced.get("Parameters") or {}).get("RuntimeEntries[1]", {}).get("ParameterInfos", []):
            names.add(i.get("Name"))
    names.discard(None)
    return names, parent


def forever(e):  # Cascade lifetime 0 or FLT_MAX = the particle never dies
    life = e.get("life")
    return bool(life) and (life[1] == 0 or life[0] > 1e30)


def usable(s):
    """A world marker candidate: Cascade sprites only, continuous or looping, small, no light."""
    if s["kind"] != "cascade" or not s["emitters"] or len(s["emitters"]) > 3:
        return False
    for e in s["emitters"]:
        if e.get("type") or e.get("light") or (e.get("volume") or 0) > 150 or ((e.get("size") or [0, 0])[1] > 40):
            return False
    live = [e for e in s["emitters"] if (e["rate"] and e["rate"][1] > 0 and (not e["rate_scale"] or e["rate_scale"][1] > 0)) or e["burst"]]
    return bool(live) and all(e["loops"] in (0, None) or forever(e) for e in live)


def catalogue(paths, export, only_usable):
    """package paths -> YAML text. export(*paths) -> {path: exports} (data.py export, cached)."""
    summ = {p: cascade(x) or niagara(x) for p, x in export(*paths).items()}
    summ = {p: s for p, s in summ.items() if s}
    want = {e["mat"] for s in summ.values() if not only_usable or usable(s) for e in s["emitters"] if e.get("mat")}
    mats, todo = {m: set() for m in want}, {m: {m} for m in want}  # todo: material -> the emitter materials it serves
    for _ in range(4):  # MI -> parent -> … (vector params live on the parent material, values on the MI)
        if not todo:
            break
        got, nxt = export(*todo), {}
        for m, owners in todo.items():
            names, parent = vector_params(got.get(m, []))
            for o in owners:
                mats[o] |= names
            if parent:
                nxt.setdefault(parent, set()).update(owners)
        todo = nxt
    return emit(summ, mats, only_usable)


def emit(summ, mats, only_usable):
    out = []
    for p in sorted(summ):
        s = summ[p]
        if only_usable and not usable(s):
            continue
        out.append(f"- path: /Game/{p.removeprefix('Archon/Content/')}.{p.rsplit('/', 1)[1]}")
        out.append(f"  kind: {s['kind']}")
        if s["params"]:
            out.append("  instance_params: [" + ", ".join(f"{n}: {t}" for n, t in s["params"]) + "]")
        for e in s["emitters"]:
            if s["kind"] == "niagara":
                out.append(f"  - emitter: {e['name']}")
                continue
            mode = "burst " + "+".join(map(str, e["burst"])) if e["burst"] else f"rate {e['rate']}"
            if e["loops"] and not forever(e):
                mode += f", {e['loops']} loop(s)"
            life = "forever" if forever(e) else e.get("life")
            colour = sorted(mats.get(e["mat"], set())) if e["mat"] else []
            mat = e["mat"].rsplit("/", 1)[1] if e["mat"] else "-"
            out.append(f"  - {{{mode}, life: {life}, volume_cm: {e.get('volume', 0)}, size_cm: {e.get('size')}, material: {mat}, "
                       f"colour_params: {colour or 'none'}" + (", light: yes" if e.get("light") else "") +
                       (f", type: {e['type']}" if e.get("type") else "") + "}")
    return "\n".join(out)


def self_test():
    ex = [{"Type": "ParticleSystem", "Properties": {"Emitters": [{"ObjectPath": "x.1"}]}},
          {"Type": "ParticleSpriteEmitter", "Properties": {"LODLevels": [{"ObjectPath": "x.2"}]}},
          {"Type": "ParticleLODLevel", "Properties": {"RequiredModule": {"ObjectPath": "x.3"}, "SpawnModule": {"ObjectPath": "x.4"},
                                                      "Modules": [{"ObjectPath": "x.5"}]}},
          {"Type": "ParticleModuleRequired", "Properties": {"Material": {"ObjectPath": "Archon/Content/M.0"}}},
          {"Type": "ParticleModuleSpawn", "Properties": {"Rate": {"Table": {"Values": [3.0]}}}},
          {"Type": "ParticleModuleLifetime", "Properties": {"Lifetime": {"Table": {"Values": [1.0, 2.0]}}}},
          {"Type": "DistributionVectorParticleParameter", "Properties": {"ParameterName": "Tint"}}]
    s = cascade(ex)
    assert s["emitters"][0]["mat"] == "Archon/Content/M" and s["emitters"][0]["life"] == [1.0, 2.0]
    assert s["params"] == [("Tint", "vector/color")] and usable(s)
    names, parent = vector_params([{"Properties": {"Parent": {"ObjectPath": "P.0"}, "VectorParameterValues": [
        {"ParameterInfo": {"Name": "Color A"}}]}}])
    assert names == {"Color A"} and parent == "P"
    assert "colour_params: ['Color A']" in emit({"Archon/Content/X/Y": s}, {"Archon/Content/M": {"Color A"}}, True)
