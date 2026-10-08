#!/usr/bin/env python3
"""Fails on an engine pointer (UFunction*, UClass*, AActor*, U<widget>* …⊇) kept beyond the current call outside
core/ref.*: namespace-scope or static variables, struct/class members, containers or atomics of them. Store those as
ref::Ref / ref::Cached / ref::Fn (core/ref.hpp, #63). Locals and parameters are fine.
Rule: .claude/rules/mod-code.md. Allowlist: ALLOW below, by file:symbol. `--self-test` runs the negative test."""
import re
import sys
from pathlib import Path

ALLOW = {  # file (relative to mods/dos-tool) -> variables or struct names that never outlive one call
    "features/inventory/item-sort/item-sort.cpp": {"Where2"},  # Locate() result, used within the caller
    "features/dungeon/shared/plan.cpp": {"Floor"},  # one floor read inside Compute()
    "core/reflect.hpp": {"Value"},  # reflect::Walk result, used within the caller's command
}
ENGINE = re.compile(r"(?<![\w:])(?:SDK::)?([AU][A-Z]\w*[a-z]\w*)\s*\*")


def strip(src):
    src = re.sub(r"//[^\n]*|/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r'"(?:\\.|[^"\\\n])*"' + r"|'(?:\\.|[^'\\\n])*'", '""', src)
    return re.sub(r"^\s*#[^\n]*", " ", src, flags=re.M)


def no_parens(s):
    out, depth = [], 0
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(ch)
    return "".join(out)


def symbols(stmt):
    return re.findall(r"[*&\s>]\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=|,|;|\{|$)", stmt)


def violations(src):
    """[(line, statement)] of kept engine pointers in one C++ source."""
    src = strip(src)
    scopes, text, start, found = [], "", 1, []  # scopes: (kind, name), kind = ns | type | code
    line = 1

    def kept():
        return (scopes[-1][0] if scopes else "ns") in ("ns", "type")

    def check(stmt, at, header=False):
        s = no_parens(stmt).strip()
        if not s or s.startswith(("using ", "typedef ", "return ", "friend ")) or (header and stmt.rstrip().endswith(")")):
            return
        if header and re.search(r"\)\s*(const|override|noexcept|final|\s)*$", stmt):
            return  # function definition
        paren, eq = stmt.find("("), stmt.find("=")
        if paren >= 0 and (eq < 0 or paren < eq):
            return  # function declaration
        static = re.match(r"(?:inline\s+|thread_local\s+|constexpr\s+)*static\b", s)
        if (kept() or static) and ENGINE.search(s):
            found.append((at, " ".join(s.split()), [n for k, n in scopes if k == "type"]))

    for ch in src:
        if ch == "\n":
            line += 1
        if ch == "{":
            head = re.sub(r"\btemplate\s*<[^{]*?>", " ", text.strip())
            check(head, start, header=True)
            m = re.search(r"\b(?:struct|class|union)\s+(\w+)", head)
            if re.search(r"\bnamespace\b", head):
                scopes.append(("ns", ""))
            elif m and not re.search(r"\benum\b", head) and "=" not in head:
                local = any(k == "code" for k, _ in scopes)  # a type inside a function body lives for that call
                scopes.append(("code" if local else "type", m.group(1)))
            else:
                scopes.append(("code", ""))
            text, start = "", line
        elif ch == "}":
            if scopes:
                scopes.pop()
            text, start = "", line
        elif ch == ";":
            check(text, start)
            text, start = "", line
        else:
            if not text.strip():
                start = line
            text += ch
    return found


def run(root):
    bad = []
    for p in sorted(root.rglob("*")):
        rel = p.relative_to(root).as_posix()
        if p.suffix not in (".cpp", ".hpp") or "/test/" in f"/{rel}" or rel.startswith("core/ref."):
            continue
        if not rel.startswith(("core/", "features/")):
            continue
        for at, stmt, types in violations(p.read_text(errors="replace")):
            if any(s in ALLOW.get(rel, ()) for s in symbols(stmt) + types):
                continue
            bad.append(f"{rel}:{at}: {stmt}")
    return bad


def self_test():
    flagged = """
namespace {
    UFunction* g_fn = nullptr;
    UClass *g_a, *g_b;
    std::unordered_map<UFunction*, std::string> g_triggers;
    std::atomic<AActor*> g_hero{nullptr};
    UFunction* g_arr[32] = {};
    struct Fns { UFunction *create, *add; } g_tbl{};
    struct Placed { UImage* img; int32 idx; };
    struct F : feature::Feature { AActor* last = nullptr; };
    void f() { static UFunction* fn = UX::StaticClass()->GetFunction("X", "Y"); }
    template <class F> struct Box { UObject* o; };
}
"""
    clean = """
#include "x.hpp"
namespace {
    ref::Fn g_fn{UX::StaticClass, "X", "Y"};
    ref::Cached<UClass> g_cls{[] { return UX_C::StaticClass(); }};
    std::unordered_map<ref::Ref, int, ref::Hash> g_seen;
    UFunction* Lookup(UObject* o, const char* n) { return o->Class->GetFunction("X", n); }
    bool Alive(const SDK::UObject* o, int32_t idx);
    using PE = void (*)(const UObject*, UFunction*, void*);
    void g(UObject* o) {
        UFunction* fn = Lookup(o, "n");
        AActor* best = nullptr;
        for (UField* f = o->Class->Children; f; f = f->Next) {}
        struct Moved { UWidget* w; int n; };
    }
    template <class F> void Each(F&& fn) { UWorld* w = UWorld::GetWorld(); }
    SDK::APlayerController* LocalPC();
    // UFunction* g_comment = nullptr;
}
"""
    got = violations(flagged)
    assert len(got) == 10, got
    assert violations(clean) == [], violations(clean)
    print("ref-check self-test: ok")


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        self_test()
        sys.exit(0)
    bad = run(Path(__file__).resolve().parent.parent / "mods" / "dos-tool")
    if bad:
        print("engine pointers kept beyond the call outside core/ref (store a ref::Ref/Cached/Fn, #63):")
        print("\n".join(bad))
        sys.exit(1)
