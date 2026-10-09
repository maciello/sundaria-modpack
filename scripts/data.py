#!/usr/bin/env python3
"""Offline game data from the pak (CUE4Parse via scripts/data/). Skill ref: references/game-data.md.
  find <regex> [--class <regex>]   asset paths (class from AssetRegistry.bin)
  show <asset>                     exports as JSON
  table <asset>                    DataTable / CurveTable rows as YAML
  bp <asset> [function]            Blueprint bytecode as readable statements
  grep <regex> [--in <path regex>] search exported values (exports matching --in first, else the cache only)
  callers <Function>               who calls it (final/virtual/broadcast) or binds it to a delegate; ProcessEvent or not
  writers|readers <Property>       Blueprint statements that assign / read an instance or struct field
  calls <BP>[::Function] [--depth N]  outgoing call graph (default depth 2); natives are leaves
  events <BP>                      event entry points (ubergraph offsets) and delegate bindings
  ast <BP>::<Function>             function bytecode as JSON AST
  fx <path regex> [--all]          particle templates for core/fx.hpp (usable ones; --all every system), YAML
The xref commands build an index over every Blueprint once per pak (xref.sqlite in DATA_CACHE; first run ~minutes).
<asset> = pak path, /Game/… path or a unique file name. Lazy: each asset is exported once into DATA_CACHE.
Env: PAKS (Content/Paks), DATA_CACHE (export/, script/ = logic-only exports, xref.sqlite), DATA_TOOLS (key, .NET SDK, built reader; outside the repo)."""
import json
import os
import re
import shutil
import subprocess
import sys
import time
from multiprocessing import Pool
from pathlib import Path

import fxcat
import xref

HERE = Path(__file__).resolve().parent
OUTER = Path(subprocess.run(["git", "-C", str(HERE), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                            capture_output=True, text=True, check=True).stdout.strip()).parent.parent
TOOLS = Path(os.environ.get("DATA_TOOLS") or OUTER / "tools")
CACHE = Path(os.environ.get("DATA_CACHE") or OUTER / "data-cache")
PAKS = Path(os.environ.get("PAKS", "."))
KEY, BIN = TOOLS / "sundaria-aes.key", TOOLS / "data-bin"
DLL = BIN / "sundaria-data.dll"
GREP_MAX = 5000  # ponytail: grep exports at most this many assets per call; narrow --in for more


def pak():
    return next(PAKS.glob("*.pak"), None) or sys.exit(f"!! no .pak in {PAKS} (set GAME_WIN64, see justfile top)")


def dotnet():
    local = TOOLS / "dotnet" / "dotnet"
    return str(local) if local.exists() else shutil.which("dotnet") or sys.exit(
        "!! no .NET 10 SDK: `just data-setup` installs one into ../tools/dotnet")


def reader(*args, out=None):
    env = dict(os.environ, PAKS=str(PAKS), AES_KEY_FILE=str(KEY), OODLE_DIR=str(BIN), DOTNET_CLI_TELEMETRY_OPTOUT="1",
               DOTNET_NOLOGO="1", DOTNET_ROOT=str(Path(dotnet()).parent))
    src = max(f.stat().st_mtime for f in (HERE / "data").glob("*.c*"))
    if not DLL.exists() or DLL.stat().st_mtime < src:
        subprocess.run([dotnet(), "build", str(HERE / "data"), "-c", "Release", "-o", str(BIN), "-v", "q", "--nologo", "--disable-build-servers"],
                       env=env, check=True, stdout=sys.stderr)
    if not KEY.exists():
        exe = next((PAKS / "../../Binaries/Win64").glob("*-Shipping.exe"))
        key = subprocess.run([dotnet(), str(DLL), "key", str(exe), str(pak())], env=env, capture_output=True, text=True)
        if key.returncode:
            sys.exit(key.stderr)
        KEY.parent.mkdir(parents=True, exist_ok=True)
        KEY.touch(mode=0o600)
        KEY.write_text(key.stdout.strip())
        print(f"key: found in {exe.name}, stored outside the repo", file=sys.stderr)
    return subprocess.run([dotnet(), str(DLL), *args], env=env, stdout=out, text=True)


def cache():
    """Cache dir for the installed pak; a game patch (new pak size/mtime) starts a fresh one."""
    st = pak().stat()
    stamp = f"{st.st_size}-{int(st.st_mtime)}"
    if (CACHE / "stamp").exists() and (CACHE / "stamp").read_text() != stamp:
        shutil.rmtree(CACHE)
    CACHE.mkdir(parents=True, exist_ok=True)
    (CACHE / "stamp").write_text(stamp)
    return CACHE


def listing(name, cmd):
    f = cache() / name
    if not f.exists():
        with open(f.with_suffix(".tmp"), "w") as out:
            if reader(cmd, out=out).returncode:
                sys.exit(f"!! reader {cmd} failed")
        f.with_suffix(".tmp").rename(f)
    return f.read_text().splitlines()


def assets():
    return [p.rsplit(".", 1)[0] for p in listing("paths.txt", "paths") if p.endswith((".uasset", ".umap"))]


def resolve(name):
    """pak path, /Game/… path, or unique file name -> package path without extension."""
    name = re.sub(r"\.(uasset|uexp|umap|json)$", "", name.split(".")[0] if name.startswith("/") else name)
    name = re.sub(r"^/Game/", "Archon/Content/", name).lstrip("/")
    all_ = assets()
    if name in all_:
        return name
    hits = [a for a in all_ if a.lower().endswith("/" + name.lower())]
    if len(hits) != 1:
        sys.exit(f"!! {name}: {len(hits)} matches" + "".join("\n  " + h for h in hits[:20]))
    return hits[0]


def export(*paths, mode="export"):
    """package paths -> loaded JSON exports, exporting cache misses in one reader call (mode script: logic exports only)."""
    out = cache() / mode
    miss = [p for p in paths if not (out / (p + ".json")).exists()]
    for i in range(0, len(miss), 200):  # argv limit
        reader(mode, str(out), *miss[i:i + 200], out=subprocess.DEVNULL)
    if mode == "script":
        return
    return {p: json.loads((out / (p + ".json")).read_text()) for p in paths if (out / (p + ".json")).exists()}


def one(path):
    return export(path).get(path) or sys.exit(f"!! {path}: export failed (see above)")


def find(rx, cls=None):
    if cls:
        for line in listing("registry.txt", "registry"):
            obj, c = line.split("\t")
            if re.search(rx, obj, re.I) and re.search(cls, c, re.I):
                print(f"{obj.split('.')[0]}\t{c}")
    else:
        print("\n".join(a for a in assets() if re.search(rx, a, re.I)))


def unguid(x):
    """BP struct field names carry `_<n>_<GUID>`: DamageModifier_5_5225… -> DamageModifier."""
    if isinstance(x, dict) and "SourceString" in x:  # FText -> its text
        return x.get("LocalizedString") or x["SourceString"]
    if isinstance(x, dict):
        return {re.sub(r"_\d+_[0-9A-F]{32}$", "", k): unguid(v) for k, v in x.items()}
    return [unguid(v) for v in x] if isinstance(x, list) else x


def table(path):
    for e in one(path):
        if "Rows" in e:
            e["Rows"] = unguid(e["Rows"])
            try:
                import yaml
                print(yaml.safe_dump({e["Name"]: e["Rows"]}, sort_keys=False, allow_unicode=True, width=120))
            except ImportError:
                print(json.dumps({e["Name"]: e["Rows"]}, indent=1))
            return
    sys.exit(f"!! {path}: no DataTable/CurveTable export (try `show`)")


# --- Kismet bytecode -> statements -------------------------------------------------------------
def short(ref):
    """{'ObjectName': "Function'Lib:Name'"} or 'Name' -> 'Lib:Name'."""
    if isinstance(ref, str):
        return ref
    m = re.match(r"\w+'(?:Default__)?(.*)'$", ref.get("ObjectName", ""))
    return m.group(1) if m else json.dumps(ref)


def var(v):
    return v["Property"]["Name"] if "Property" in v else ".".join(v.get("Path", ["?"]))


def call(fn, params):
    name = short(fn).split(":")[-1]
    return f"{name}({', '.join(expr(p) for p in params)})"


def val(v):
    """any JSON field of a token -> text: nested tokens, object refs, properties, plain values."""
    if isinstance(v, list):
        return f"[{', '.join(val(x) for x in v)}]"
    if isinstance(v, dict):
        if "Token" in v:
            return expr(v)
        if "ObjectName" in v:
            return short(v)
        if "Property" in v or "Path" in v:
            return var(v)
        if "SourceString" in v:
            return json.dumps(v.get("LocalizedString") or v["SourceString"])
        return "{" + ", ".join(f"{k}: {val(x)}" for k, x in v.items()) + "}"
    return json.dumps(v) if isinstance(v, str) else str(v)


CALLS = ("CallMath", "FinalFunction", "LocalFinalFunction", "VirtualFunction", "LocalVirtualFunction")
LETS = ("Let", "LetBool", "LetObj", "LetWeakObjPtr", "LetDelegate", "LetMulticastDelegate")
CASTS = ("Cast", "DynamicCast", "MetaCast", "ObjToInterfaceCast", "InterfaceToObjCast", "CrossInterfaceCast")
WORDS = {"True": "true", "False": "false", "Self": "self", "NoObject": "null", "Nothing": "∅",
         "PopExecutionFlow": "pop", "EndOfScript": "end"}


def expr(e):
    t = e.get("Token", "")[3:]
    if t in ("LocalVariable", "InstanceVariable", "LocalOutVariable", "DefaultVariable"):
        return ("self." if t == "InstanceVariable" else "") + var(e["Variable"])
    if t in WORDS:
        return WORDS[t]
    if t.endswith("Const") and "Value" in e:
        return val(e["Value"])
    if t in CALLS:
        return call(e["Function"], e.get("Parameters", []))
    if t in ("Context", "Context_FailSilent", "ClassContext"):
        return f"{expr(e['ObjectExpression'])}.{expr(e['ContextExpression'])}"
    if t == "InterfaceContext":
        return expr(e["InterfaceValue"])
    if t == "StructMemberContext":
        return f"{expr(e['StructExpression'])}.{var(e['Property'])}"
    if t == "ArrayGetByRef":
        return f"{expr(e['ArrayVariable'])}[{expr(e['ArrayIndex'])}]"
    if t == "StructConst":
        return f"{short(e['Struct'])}{{{', '.join(expr(p) for p in e.get('Properties', []))}}}"
    if t in CASTS:
        return f"{t}<{short(e['InterfaceClass']) if 'InterfaceClass' in e else e.get('ConversionType')}>({expr(e['Target'])})"
    if t in LETS:
        return f"{val(e['Variable'])} = {expr(e['Expression'])}"
    if t == "LetValueOnPersistentFrame":
        return f"{var(e['DestinationProperty'])} = {expr(e['AssignmentExpression'])}"
    if t == "SetArray":
        return f"{expr(e['AssigningProperty'])} = {val(e['Elements'])}"
    if t == "Return":
        return f"return {expr(e['Expression'])}"
    if t == "Jump":
        return f"goto {e['CodeOffset']}"
    if t == "ComputedJump":
        return f"goto {expr(e['CodeOffsetExpression'])}"
    if t == "JumpIfNot":
        return f"if not {expr(e['BooleanExpression'])}: goto {e['CodeOffset']}"
    if t == "PushExecutionFlow":
        return f"push {e['PushingAddress']}"
    if t == "PopExecutionFlowIfNot":
        return f"if not {expr(e['BooleanExpression'])}: pop"
    rest = [f"{k}={val(v)}" for k, v in e.items() if k not in ("Token", "StatementIndex", "ObjectPath")]
    return f"{t}({', '.join(rest)})"  # generic: every field, nested tokens rendered


def bp(path, fn=None):
    for e in one(path):
        if "ScriptBytecode" in e and (not fn or re.fullmatch(fn, e["Name"], re.I)):
            args = [f"{p['Name']}: {p['Type'].replace('Property', '')}" for p in e.get("ChildProperties", [])
                    if "Parm" in p.get("PropertyFlags", "")]
            print(f"\n{short(e['Outer']) if 'Outer' in e else path}::{e['Name']}({', '.join(args)})")
            for s in e["ScriptBytecode"]:
                print(f"  {s.get('StatementIndex', ''):>5}  {expr(s)}")


def flat(x, at=""):
    if isinstance(x, dict):
        for k, v in x.items():
            yield from flat(v, f"{at}.{k}" if at else k)
    elif isinstance(x, list):
        for i, v in enumerate(x):
            yield from flat(v, f"{at}[{i}]")
    else:
        yield at, x


def grep(rx, within=None):
    if within:
        todo = [a for a in assets() if re.search(within, a, re.I)]
        if len(todo) > GREP_MAX:
            print(f"!! {len(todo)} assets match --in, exporting the first {GREP_MAX}", file=sys.stderr)
        export(*todo[:GREP_MAX])
    root = cache() / "export"
    for f in sorted(root.rglob("*.json")):
        path = str(f.relative_to(root))[:-5]
        if within and not re.search(within, path, re.I):
            continue
        for k, v in flat(json.loads(f.read_text())):
            if re.search(rx, f"{k} = {v}", re.I):
                print(f"{path}  {k} = {v}")


def bp_packages():
    """every package holding a Blueprint class (registry) -> pak package path (/Game, /Engine, plugin mounts)."""
    mount = {}
    for a in assets():
        m = re.match(r"(?:.*?/)?(\w+)/Content/(.*)", a)
        if m:
            mount.setdefault(f"/{'Game' if m[1] == 'Archon' else m[1]}/{m[2]}", a)
    pk = {ln.split("\t")[0].split(".")[0] for ln in listing("registry.txt", "registry") if ln.endswith("GeneratedClass")}
    return sorted(mount[p] for p in pk if p in mount)


def _rows(pkg):
    f = CACHE / "script" / (pkg + ".json")
    try:
        return pkg, xref.rows(pkg, json.loads(f.read_text())) if f.exists() else {}
    except Exception as e:  # one odd token shape must not stop the index; the package stays unindexed
        return pkg, f"{type(e).__name__}: {e}"


def index():
    """xref db for the installed pak; indexes Blueprints not indexed yet (cold: all, resumable)."""
    con = xref.connect(cache() / "xref.sqlite")
    done = {r[0] for r in con.execute("select pkg from done")}
    todo = [p for p in bp_packages() if p not in done]
    if todo:
        t0 = time.time()
        print(f"xref: indexing {len(todo)} Blueprints (once per pak)", file=sys.stderr)
        for i in range(0, len(todo), 1000):
            export(*todo[i:i + 1000], mode="script")
            with Pool() as pool:
                for pkg, r in pool.imap_unordered(_rows, todo[i:i + 1000], chunksize=4):
                    if isinstance(r, str):
                        print(f"!! xref {pkg}: {r}", file=sys.stderr)
                    else:
                        xref.store(con, pkg, r)
            con.commit()
        print(f"xref: {len(todo)} Blueprints in {time.time() - t0:.0f} s", file=sys.stderr)
    return con


def ast(con, target):
    cls, fn = (target.split("::") + [""])[:2]
    cls, pkg = xref.bp_class(con, cls)
    export(pkg, mode="script")
    fns = [e for e in json.loads((CACHE / "script" / (pkg + ".json")).read_text()) if "ScriptBytecode" in e and e["Name"].lower() == fn.lower()]
    print(json.dumps(xref.slim(fns[0]["ScriptBytecode"]) if fns else sys.exit(f"!! {cls} has no function {fn}"), indent=1))


def self_test():
    """game-free check of the bytecode printer on a hand-written token tree."""
    loc = lambda n: {"Token": "EX_LocalVariable", "Variable": {"Property": {"Name": n}}}
    mul = {"Token": "EX_CallMath", "Function": {"ObjectName": "Function'KismetMathLibrary:Multiply_FloatFloat'"},
           "Parameters": [loc("a"), {"Token": "EX_FloatConst", "Value": 2.0}]}
    assert expr({"Token": "EX_Let", "Variable": loc("r"), "Expression": mul}) == "r = Multiply_FloatFloat(a, 2.0)"
    assert expr({"Token": "EX_JumpIfNot", "BooleanExpression": loc("c"), "CodeOffset": 9}) == "if not c: goto 9"
    assert expr({"Token": "EX_Foo", "X": loc("a")}) == "Foo(X=a)"
    assert unguid({"Dmg_5_52250031437C09A7C029A59A92FB49DC": {"SourceString": "s"}}) == {"Dmg": "s"}
    print("data.py self-test ok")
    xref.self_test()
    fxcat.self_test()


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    opts = {a: argv[i + 1] for i, a in enumerate(argv) if a.startswith("--") and i + 1 < len(argv)}
    pos = [a for i, a in enumerate(argv) if not a.startswith("--") and not (i and argv[i - 1].startswith("--"))]
    if len(pos) < 2:
        sys.exit(__doc__)
    cmd, arg = pos[0], pos[1]
    if cmd == "find":
        find(arg, opts.get("--class"))
    elif cmd == "show":
        print(json.dumps(one(resolve(arg)), indent=1))
    elif cmd == "table":
        table(resolve(arg))
    elif cmd == "bp":
        bp(resolve(arg), pos[2] if len(pos) > 2 else None)
    elif cmd == "grep":
        grep(arg, opts.get("--in"))
    elif cmd == "callers":
        xref.callers(index(), arg)
    elif cmd in ("writers", "readers"):
        xref.access(index(), arg, int(cmd == "writers"))
    elif cmd == "calls":
        con, (bp_, fn) = index(), (arg.split("::") + [""])[:2]
        cls = xref.bp_class(con, bp_)[0]
        for (f,) in con.execute("select name from fn where cls = ? and (name = ? collate nocase or ? = '' and "
                                "name not like 'ExecuteUbergraph%') order by name", (cls, fn, fn)):
            print(f"{cls}::{f}")
            xref.calls(con, cls, f, int(opts.get("--depth", 2)), pad="  ")
    elif cmd == "events":
        con = index()
        xref.events(con, xref.bp_class(con, arg)[0])
    elif cmd == "ast":
        ast(index(), arg)
    elif cmd == "fx":
        systems = [re.sub(r"^/Game/", "Archon/Content/", obj.split(".")[0]) for obj, c in
                   (line.split("\t") for line in listing("registry.txt", "registry"))
                   if c in ("ParticleSystem", "NiagaraSystem") and re.search(arg, obj, re.I)]
        print(fxcat.catalogue(sorted(set(systems)), export, "--all" not in argv))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
