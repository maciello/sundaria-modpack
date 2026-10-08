#!/usr/bin/env python3
"""Query the Dumper-7 CppSDK without reading 140 MB of headers.

  sdk.py class  AArchonCharacter       # class/struct body with offsets, file:line
  sdk.py chain  AArchonCharacter       # inheritance chain to UObject
  sdk.py field  'AirControl|Health'    # regex over member names -> owner::member @offset
  sdk.py subs   AArchonCharacter       # every (transitive) subclass in the dump
SDK dir: $SDK_DIR or <repo>/../sdk/CppSDK.
"""
import os, re, sys
from pathlib import Path

repo = Path(__file__).resolve().parents[4]
sdk = Path(os.environ.get("SDK_DIR", repo.parent / "sdk/CppSDK")) / "SDK"
HEAD = re.compile(r"^(?:class|struct) (?:SDK_ALIGN\(\w+\) )?(\w+)(?: final)?(?: : public (\w+))?\s*$")
MEMB = re.compile(r"^\s+(.+?)\s+(\w+)(?:\[\w+\])?;\s+// (0x[0-9A-F]+)\((0x[0-9A-F]+)\)")


def blocks():
    for f in sorted(sdk.glob("*_classes.hpp")) + sorted(sdk.glob("*_structs.hpp")):
        lines = f.read_text(errors="replace").splitlines()
        i = 0
        while i < len(lines):
            m = HEAD.match(lines[i])
            if m:
                j = i
                while j < len(lines) and lines[j] != "};":
                    j += 1
                yield m[1], m[2], f, i + 1, lines[i:j + 1]
                i = j
            i += 1


def main(cmd, arg):
    if not sdk.is_dir():
        sys.exit(f"no SDK at {sdk} (set SDK_DIR)")
    if cmd == "class":
        for name, _, f, ln, body in blocks():
            if name == arg:
                print(f"{f.name}:{ln}")
                print("\n".join(l for l in body if "DUMPER7" not in l))
                return
    elif cmd in ("chain", "subs"):
        parent = {n: p for n, p, *_ in blocks()}
        if cmd == "chain":
            n = arg
            while n:
                print(n)
                n = parent.get(n)
            return
        kids = sorted(n for n in parent if n != arg and arg in _up(parent, n))
        print("\n".join(kids))
        print(f"# {len(kids)} subclasses of {arg} in this dump (Blueprint classes only if loaded at dump time)")
        return
    elif cmd == "field":
        rx = re.compile(arg)
        for name, _, f, ln, body in blocks():
            for k, l in enumerate(body):
                m = MEMB.match(l)
                if m and rx.search(m[2]):
                    print(f"{name}::{m[2]} @{m[3]} ({m[1].strip()})  {f.name}:{ln + k}")
        return
    sys.exit(__doc__)


def _up(parent, n):
    out = []
    while n in parent and parent[n]:
        n = parent[n]
        out.append(n)
    return out


if __name__ == "__main__":
    main(*(sys.argv[1:3] if len(sys.argv) >= 3 else ("", "")))
