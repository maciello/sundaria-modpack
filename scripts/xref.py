"""Cross-reference index over Blueprint Kismet bytecode (sqlite). Pure: rows from export JSON, queries on the db.
Front end: scripts/data.py (callers|writers|readers|calls|events|ast). Skill ref: references/game-data.md.

Call kinds (UE 4.27 ScriptCore.cpp): every script call opcode ends in UObject::CallFunction / ProcessLocalFunction,
which runs a native thunk (Function->Invoke) or ProcessScriptFunction directly; none enters UObject::ProcessEvent.
  final   EX_FinalFunction / EX_LocalFinalFunction / EX_CallMath: UFunction* fixed at load
  virtual EX_VirtualFunction / EX_LocalVirtualFunction: FindFunctionChecked(name) at run time (child overrides win)
  bcast   EX_CallMulticastDelegate: ProcessMulticastDelegate -> ProcessDelegate -> ProcessEvent on every bound function
ProcessEvent is entered only from outside the script VM: delegate broadcasts (bindings below), native code calling a
BlueprintImplementableEvent/NativeEvent (BP side: FUNC_Event), RPCs, timers, input."""
import re
import sqlite3

SCHEMA = """
create table if not exists done(pkg text primary key);
create table if not exists cls(pkg, name, super);
create table if not exists fn(pkg, cls, name, flags, entry);
create table if not exists call(pkg, cls, fn, off, ev, kind, op, owner, name, native, via);
create table if not exists var(pkg, cls, fn, off, ev, rw, owner, name);
create table if not exists bind(pkg, cls, fn, off, ev, op, delegate, owner, target, obj);
create index if not exists call_name on call(name collate nocase);
create index if not exists call_from on call(cls, fn);
create index if not exists var_name on var(name collate nocase);
create index if not exists bind_target on bind(target collate nocase);
create index if not exists bind_cls on bind(cls);
create index if not exists fn_cls on fn(cls, name);
create index if not exists fn_name on fn(name collate nocase);
create index if not exists cls_name on cls(name collate nocase);
"""
KIND = {"EX_FinalFunction": "final", "EX_LocalFinalFunction": "final", "EX_CallMath": "final",
        "EX_VirtualFunction": "virtual", "EX_LocalVirtualFunction": "virtual", "EX_CallMulticastDelegate": "bcast"}
MUTATES = re.compile(r"(Array|Map|Set)_(Add|AddUnique|AddItems|Append|Clear|Insert|Remove|RemoveItem|RemoveItems|Resize|Set|Shuffle|Swap)$")
VARS = ("EX_InstanceVariable", "EX_DefaultVariable")  # locals/temps are not indexed
LETS = ("EX_Let", "EX_LetBool", "EX_LetObj", "EX_LetWeakObjPtr")
BINDS = {"EX_AddMulticastDelegate": "add", "EX_RemoveMulticastDelegate": "remove", "EX_ClearMulticastDelegate": "clear",
         "EX_LetDelegate": "set", "EX_LetMulticastDelegate": "set"}


def ref(r):
    """{'ObjectName': "Function'Owner:Name'", 'ObjectPath': …} -> (owner, name, native)."""
    m = re.match(r"\w+'(?:Default__)?(.*)'$", r.get("ObjectName", ""))
    owner, _, name = (m.group(1) if m else "?").rpartition(":")
    return owner, name, int(r.get("ObjectPath", "").startswith("/Script/"))


def prop(v):
    """Variable/Property field -> (owner class, name), GUID suffix of BP struct fields stripped."""
    if isinstance(v, dict) and "Token" in v:
        return prop(v.get("Variable") or {})
    v = v or {}
    if "Path" in v:
        return ref(v.get("ResolvedOwner") or {})[1] or "?", v["Path"][-1]
    p = v.get("Property", v)
    return ref(v.get("Owner") or {})[1] or "?", re.sub(r"_\d+_[0-9A-F]{32}$", "", p.get("Name", "?"))


def static_class(e):
    """class of the object an expression yields, when the bytecode says (property class), else ''."""
    if e.get("Token") == "EX_ObjectConst":  # function library CDO: Default__BP_Lib_C
        return ref(e.get("Value") or {})[1]
    v = e.get("Variable") or {}
    if e.get("Token") == "EX_InterfaceContext":
        return static_class(e.get("InterfaceValue") or {})
    p = v.get("Property", {}) if isinstance(v, dict) else {}
    pc = p.get("PropertyClass") or p.get("InterfaceClass")
    if isinstance(pc, dict):
        return ref(pc)[1]
    if e.get("Token") == "EX_Context":
        return static_class(e.get("ContextExpression", {}))
    return ""


def leaf(e):
    """innermost variable of a (Context/StructMember) chain, for delegate targets."""
    while e.get("Token") == "EX_Context":
        e = e["ContextExpression"]
    return prop(e) if "Variable" in e else ("?", "?")


def successors(st, i):
    """CFG edges of statement i (offsets): jumps, push/pop flow, latent resume (LatentActionInfo.Linkage)."""
    t, nxt = st[i]["Token"], [st[i + 1]["StatementIndex"]] if i + 1 < len(st) else []
    if t == "EX_Jump":
        return [st[i]["CodeOffset"]]
    if t == "EX_JumpIfNot":
        return nxt + [st[i]["CodeOffset"]]
    if t == "EX_PushExecutionFlow":
        return nxt + [st[i]["PushingAddress"]]
    if t in ("EX_Return", "EX_EndOfScript", "EX_PopExecutionFlow", "EX_ComputedJump"):
        return []
    return nxt + list(latent(st[i]))


def latent(e):
    """resume offsets of latent calls in e (FLatentActionInfo.Linkage, first field of the struct const)."""
    if isinstance(e, list):
        for x in e:
            yield from latent(x)
    elif isinstance(e, dict):
        if e.get("Token") == "EX_StructConst" and "LatentActionInfo" in e.get("Struct", {}).get("ObjectName", ""):
            yield e["Properties"][0]["Value"]
        for v in e.values():
            yield from latent(v)


def reach(st, entries):
    """offset -> ',ev1,ev2,' : which event entry points reach each ubergraph statement."""
    at = {s["StatementIndex"]: i for i, s in enumerate(st)}
    ev = {s["StatementIndex"]: set() for s in st}
    for name, off in entries:
        todo, seen = [off], set()
        while todo:
            o = todo.pop()
            if o in seen or o not in at:
                continue
            seen.add(o)
            ev[o].add(name)
            todo += successors(st, at[o])
    return {o: "," + ",".join(sorted(n)) + "," if n else "" for o, n in ev.items()}


def rows(pkg, exports):
    """one package's exports -> {'cls'|'fn'|'call'|'var'|'bind': [tuple…]}."""
    out = {k: [] for k in ("cls", "fn", "call", "var", "bind")}
    funcs = [e for e in exports if "ScriptBytecode" in e]
    for e in exports:
        if e.get("Type", "").endswith("GeneratedClass"):
            out["cls"].append((pkg, e["Name"], ref(e.get("SuperStruct") or {})[1]))
        for b in (e.get("Properties") or {}).get("ComponentDelegateBindings", []) if "DelegateBinding" in e.get("Type", "") else []:
            out["bind"].append((pkg, ref(e["Outer"])[1], "", -1, "", "component", b["DelegatePropertyName"], "",
                                b["FunctionNameToBind"], b["ComponentPropertyName"]))
    entries = {}  # ubergraph name -> [(event, offset)]
    for f in funcs:
        for s in f["ScriptBytecode"]:
            fn, p = s.get("Function"), s.get("Parameters", [])
            if isinstance(fn, dict) and ref(fn)[1].startswith("ExecuteUbergraph_") and p and "Value" in p[0]:
                entries.setdefault(ref(fn)[1], []).append((f["Name"], p[0]["Value"]))
                f["_entry"] = p[0]["Value"]
    for f in funcs:
        cls, name, st = ref(f["Outer"])[1], f["Name"], f["ScriptBytecode"]
        out["fn"].append((pkg, cls, name, f.get("FunctionFlags", ""), f.get("_entry")))
        evs = reach(st, entries.get(name, [])) if name in entries else {}
        pending = {}

        def visit(e, off, ev, rw=0, via=""):
            if isinstance(e, list):
                for x in e:
                    visit(x, off, ev)
                return
            if not isinstance(e, dict):
                return
            t = e.get("Token", "")
            if t in KIND:
                f_ = e.get("Function") if t != "EX_CallMulticastDelegate" else e.get("FunctionName")
                if t == "EX_CallMulticastDelegate":
                    owner, dname = leaf(e["Delegate"])
                    out["call"].append((pkg, cls, name, off, ev, "bcast", t[3:], owner, dname, 0, ref(f_)[1]))
                    visit(e["Delegate"], off, ev)
                elif isinstance(f_, dict):
                    o, n, nat = ref(f_)
                    out["call"].append((pkg, cls, name, off, ev, KIND[t], t[3:], o, n, nat, via or (cls if "Local" in t else "")))
                else:
                    out["call"].append((pkg, cls, name, off, ev, "virtual", t[3:], "", f_, 0, via or (cls if "Local" in t else "")))
                ps = e.get("Parameters", [])
                if ps and MUTATES.match(out["call"][-1][8] or ""):  # container op: its first argument is written
                    visit(ps[0], off, ev, 1)
                    ps = ps[1:]
                visit(ps, off, ev)
                return
            if t in VARS:
                out["var"].append((pkg, cls, name, off, ev, rw, *prop(e["Variable"])))
                return
            if t == "EX_StructMemberContext":
                out["var"].append((pkg, cls, name, off, ev, rw, *prop(e["Property"])))
                visit(e["StructExpression"], off, ev, rw)
                return
            if t in ("EX_Context", "EX_Context_FailSilent", "EX_ClassContext"):
                visit(e["ObjectExpression"], off, ev)
                visit(e["ContextExpression"], off, ev, rw, static_class(e["ObjectExpression"]) or "?")
                return
            if t in LETS or t == "EX_LetValueOnPersistentFrame":
                visit(e.get("Variable") or {"Token": "EX_InstanceVariable", "Variable": e["DestinationProperty"]}, off, ev, 1)
                visit(e.get("Expression") or e.get("AssignmentExpression"), off, ev)
                return
            if t == "EX_BindDelegate":
                pending[leaf(e["Delegate"])[1]] = (e["FunctionName"], static_class(e["ObjectTerm"]) or
                                                   ("self" if e["ObjectTerm"].get("Token") == "EX_Self" else "?"))
                return
            if t in BINDS:
                d = e.get("MulticastDelegate") or e.get("Variable") or e.get("DelegateToClear") or {}
                owner, dname = leaf(d)
                if d.get("Token") == "EX_Context":
                    dname = f"{leaf(d['ObjectExpression'])[1] if 'Variable' in d['ObjectExpression'] else d['ObjectExpression'].get('Token', '?')[3:]}.{dname}"
                src = e.get("Delegate") if t != "EX_LetDelegate" else e.get("Expression")
                target, obj = pending.get(leaf(src)[1], ("", "")) if isinstance(src, dict) else ("", "")
                out["bind"].append((pkg, cls, name, off, ev, BINDS[t], dname, owner, target, obj))
                return
            for k, v in e.items():
                if isinstance(v, (dict, list)):
                    visit(v, off, ev)

        for s in st:
            visit(s, s["StatementIndex"], evs.get(s["StatementIndex"], ""))
    return out


def connect(db):
    con = sqlite3.connect(db)
    con.executescript(SCHEMA)
    return con


def store(con, pkg, r):
    for t, rs in r.items():
        if rs:
            con.executemany(f"insert into {t} values ({','.join('?' * len(rs[0]))})", rs)
    con.execute("insert or ignore into done values (?)", (pkg,))


def self_test():
    """hand-written ubergraph: event stub -> entry, instance write, final/virtual call, delegate bind, reachability."""
    iv = lambda n: {"Token": "EX_InstanceVariable", "Variable": {"Owner": {"ObjectName": "BlueprintGeneratedClass'A_C'"},
                                                                  "Property": {"Name": n}}}
    lv = lambda n: {"Token": "EX_LocalVariable", "Variable": {"Property": {"Name": n}}}
    fref = lambda o, n, p="X/A": {"ObjectName": f"Function'{o}:{n}'", "ObjectPath": p}
    outer = {"ObjectName": "BlueprintGeneratedClass'A_C'"}
    uber = [{"Token": "EX_ComputedJump", "StatementIndex": 0},
            {"Token": "EX_LetBool", "StatementIndex": 10, "Variable": iv("IsShown"), "Expression": {"Token": "EX_True"}},
            {"Token": "EX_JumpIfNot", "StatementIndex": 20, "BooleanExpression": iv("Locked"), "CodeOffset": 50},
            {"Token": "EX_LocalVirtualFunction", "StatementIndex": 30, "Function": "Open", "Parameters": []},
            {"Token": "EX_Context", "StatementIndex": 35, "ObjectExpression": {"Token": "EX_ObjectConst", "Value": {
                "ObjectName": "KismetArrayLibrary'Default__KismetArrayLibrary'"}}, "ContextExpression": {"Token": "EX_FinalFunction",
                "Function": fref("KismetArrayLibrary", "Array_Add", "/Script/Engine"), "Parameters": [iv("LockTags"), lv("t")]}},
            {"Token": "EX_Return", "StatementIndex": 40},
            {"Token": "EX_BindDelegate", "StatementIndex": 50, "FunctionName": "OnHit", "Delegate": lv("D"), "ObjectTerm": {"Token": "EX_Self"}},
            {"Token": "EX_AddMulticastDelegate", "StatementIndex": 60, "Delegate": lv("D"), "MulticastDelegate": {
                "Token": "EX_Context", "ObjectExpression": iv("Mesh"), "ContextExpression": {"Token": "EX_InstanceVariable",
                "Variable": {"Path": ["OnComponentHit"], "ResolvedOwner": {"ObjectName": "Class'PrimitiveComponent'"}}}}},
            {"Token": "EX_Return", "StatementIndex": 70}]
    stub = [{"Token": "EX_LocalFinalFunction", "StatementIndex": 0, "Function": fref("A_C", "ExecuteUbergraph_A"),
             "Parameters": [{"Token": "EX_IntConst", "Value": 10}]},
            {"Token": "EX_FinalFunction", "StatementIndex": 15, "Function": fref("Actor", "K2_DestroyActor", "/Script/Engine")}]
    r = rows("X/A", [{"Type": "BlueprintGeneratedClass", "Name": "A_C", "SuperStruct": {"ObjectName": "Class'Actor'"}},
                     {"Name": "ExecuteUbergraph_A", "Outer": outer, "ScriptBytecode": uber},
                     {"Name": "ReceiveBeginPlay", "Outer": outer, "FunctionFlags": "FUNC_Event", "ScriptBytecode": stub}])
    assert ("X/A", "A_C", "ReceiveBeginPlay", "FUNC_Event", 10) in r["fn"], r["fn"]
    assert ("X/A", "A_C", "ExecuteUbergraph_A", 10, ",ReceiveBeginPlay,", 1, "A_C", "IsShown") in r["var"], r["var"]
    assert ("X/A", "A_C", "ExecuteUbergraph_A", 30, ",ReceiveBeginPlay,", "virtual", "LocalVirtualFunction", "", "Open", 0, "A_C") in r["call"]
    assert ("X/A", "A_C", "ReceiveBeginPlay", 15, "", "final", "FinalFunction", "Actor", "K2_DestroyActor", 1, "") in r["call"]
    assert ("X/A", "A_C", "ExecuteUbergraph_A", 60, ",ReceiveBeginPlay,", "add", "Mesh.OnComponentHit", "PrimitiveComponent", "OnHit", "self") in r["bind"]
    assert ("X/A", "A_C", "ExecuteUbergraph_A", 35, ",ReceiveBeginPlay,", 1, "A_C", "LockTags") in r["var"], r["var"]
    assert reach(uber, [("E", 50)])[10] == "" and reach(uber, [("E", 10)])[70] == ",E,"
    delay = [{"Token": "EX_FinalFunction", "StatementIndex": 0, "Parameters": [{"Token": "EX_StructConst", "Struct": {
        "ObjectName": "ScriptStruct'LatentActionInfo'"}, "Properties": [{"Token": "EX_SkipOffsetConst", "Value": 20}]}]},
             {"Token": "EX_Return", "StatementIndex": 10}, {"Token": "EX_Return", "StatementIndex": 20}]
    assert reach(delay, [("E", 0)])[20] == ",E,"  # latent resume point belongs to the event
    con = connect(":memory:")
    store(con, "X/A", r)
    assert con.execute("select count(*) from call where name = 'k2_destroyactor' collate nocase").fetchone() == (1,)
    print("xref self-test ok")


# --- queries ---------------------------------------------------------------------------------------
PE = {"final": "script VM, no ProcessEvent", "virtual": "script VM by name (overrides win), no ProcessEvent",
      "bcast": "broadcast: each bound function is entered via ProcessEvent"}


def evs(ev, n=3):
    e = [x for x in ev.split(",") if x]
    return f" [{','.join(e[:n])}{f' +{len(e) - n}' if len(e) > n else ''}]" if e else ""


def bp_class(con, name):
    hits = con.execute("select name, pkg from cls where name in (?, ?) collate nocase", (name, name + "_C")).fetchall()
    if len(hits) != 1:
        raise SystemExit(f"!! {name}: {len(hits)} Blueprint classes" + "".join(f"\n  {n}  {p}" for n, p in hits[:20]))
    return hits[0]


def defining(con, cls, name):
    """class in cls's super chain that defines name, else None."""
    while cls:
        if con.execute("select 1 from fn where cls = ? and name = ?", (cls, name)).fetchone():
            return cls
        cls = (con.execute("select super from cls where name = ?", (cls,)).fetchone() or [None])[0]
    return None


def callers(con, name):
    for c, f, flags in con.execute("select cls, name, flags from fn where name = ? collate nocase", (name,)):
        print(f"defined  {c}::{f}  {flags}" + ("  <- native code calls it via ProcessEvent" if "FUNC_Event" in flags else ""))
    for kind in PE:
        hits = con.execute("select cls, fn, off, ev, op, owner, via from call where name = ? collate nocase and kind = ? "
                           "order by cls, fn, off", (name, kind)).fetchall()
        if hits:
            print(f"{kind}: {PE[kind]}")
        for c, f, off, ev, op, owner, via in hits:
            print(f"  {c}::{f} @{off}{evs(ev)}  {op} -> {owner or via or '?'}")
    hits = con.execute("select cls, fn, off, ev, op, delegate, owner, obj from bind where target = ? collate nocase "
                       "order by cls, fn, off", (name,)).fetchall()
    if hits:
        print("bound: delegate fires it via ProcessDelegate -> ProcessEvent")
    for c, f, off, ev, op, d, owner, obj in hits:
        print(f"  {c}::{f or '(class)'} @{off}{evs(ev)}  {op} {d}" + (f" ({owner})" if owner else "") + f" -> {obj}.{name}")


def access(con, name, rw):
    n = 0
    for c, f, off, ev, owner, p in con.execute("select cls, fn, off, ev, owner, name from var where name = ? collate nocase "
                                               "and rw = ? group by cls, fn, off, owner order by cls, fn, off", (name, rw)):
        print(f"{c}::{f} @{off}{evs(ev)}  {owner}.{p}")
        n += 1
    print(f"# {n} {'writes' if rw else 'reads'}")


def calls(con, cls, fn, depth, ev="", pad="", seen=None):
    seen = seen if seen is not None else set()
    q = ("select kind, op, owner, name, native, via, min(off) from call where cls = ? and fn = ?"
         + (" and ev like ?" if ev else "") + " group by kind, owner, name, via order by min(off)")
    for kind, op, owner, name, native, via, off in con.execute(q, (cls, fn) + ((f"%,{ev},%",) if ev else ())):
        tgt = defining(con, owner if kind == "final" else via, name) if kind != "bcast" else None
        cn = owner if kind == "final" else via
        tag = (" [native]" if native else " [ProcessEvent on bound]" if kind == "bcast" else "" if tgt else
               " [unresolved: by name]" if not cn else " [native class; a Blueprint subclass may override]"
               if not con.execute("select 1 from cls where name = ?", (cn,)).fetchone() else " [unresolved]")
        print(f"{pad}@{off} {kind} {tgt or owner or via or '?'}:{name}{tag}")
        if tgt and depth > 1 and (tgt, name, ev) not in seen:
            seen.add((tgt, name, ev))
            sub = fn if name.startswith("ExecuteUbergraph_") else ""
            calls(con, tgt, name, depth - (0 if sub else 1), sub, pad + "  ", seen)


def events(con, cls):
    sup = (con.execute("select super from cls where name = ?", (cls,)).fetchone() or ["?"])[0]
    print(f"{cls} : {sup}\nentry points (ubergraph offset; FUNC_Event = native code calls it via ProcessEvent):")
    for f, flags, entry in con.execute("select name, flags, entry from fn where cls = ? and entry is not null order by entry", (cls,)):
        print(f"  @{entry} {f}  {flags}")
    print("bindings (bound functions are entered via ProcessDelegate -> ProcessEvent):")
    for f, off, ev, op, d, owner, tgt, obj in con.execute("select fn, off, ev, op, delegate, owner, target, obj from bind "
                                                          "where cls = ? order by fn, off", (cls,)):
        print(f"  {op} {d}" + (f" ({owner})" if owner else "") + f" -> {obj}.{tgt or '?'}" +
              (f"   in {f} @{off}{evs(ev)}" if f else ""))


def slim(x):
    """export JSON -> AST without load metadata: object refs as 'Owner:Name', properties as {Name, Type, Owner}."""
    if isinstance(x, list):
        return [slim(v) for v in x]
    if not isinstance(x, dict):
        return x
    if "ObjectName" in x and len(x) <= 2:
        o, n, _ = ref(x)
        return f"{o}:{n}" if o else n
    if "Property" in x and isinstance(x["Property"], dict) or "Path" in x:
        o, n = prop(x)
        return {"Name": n, "Type": x.get("Property", {}).get("Type", ""), "Owner": o}
    return {k: slim(v) for k, v in x.items() if k not in ("ObjectPath",)}
