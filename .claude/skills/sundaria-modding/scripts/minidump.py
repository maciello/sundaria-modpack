#!/usr/bin/env python3
"""minidump.py [dmp] — fault module+offset, registers, and stack return addresses of a UE crash.
Default: newest UE4Minidump.dmp in the game's Proton prefix. Stdlib only (pip `minidump` chokes on UE's custom streams)."""
import glob, os, struct, sys

dmp = sys.argv[1] if len(sys.argv) > 1 else max(glob.glob(os.path.expanduser(
    "~/.steam/steam/steamapps/compatdata/587520/pfx/drive_c/users/steamuser/AppData/Local/Archon/Saved/Crashes/*/UE4Minidump.dmp")),
    key=os.path.getmtime)
d = open(dmp, "rb").read()
n, rva = struct.unpack_from("<II", d, 8)
streams = {}
for i in range(n):
    t, sz, r = struct.unpack_from("<III", d, rva + 12 * i)
    streams.setdefault(t, (sz, r))
mods = []
_, r = streams[4]
for i in range(struct.unpack_from("<I", d, r)[0]):
    o = r + 4 + 108 * i
    base, size = struct.unpack_from("<QI", d, o)
    nr = struct.unpack_from("<I", d, o + 20)[0]
    mods.append((base, base + size, d[nr + 4:nr + 4 + struct.unpack_from("<I", d, nr)[0]].decode("utf-16le").split("\\")[-1]))
where = lambda a: next((f"{nm}+{a - b:#x}" for b, e, nm in mods if b <= a < e), "?")
_, r = streams[6]
code, _, _, addr = struct.unpack_from("<IIQQ", d, r + 8)
ctx = struct.unpack_from("<II", d, r + 8 + 152)[1]
reg = lambda off: struct.unpack_from("<Q", d, ctx + off)[0]
rsp = reg(0x98)
print(dmp)
print(f"code {code:#x} at {where(addr)}  rcx {reg(0x80):#x} rdx {reg(0x88):#x} r8 {reg(0xB8):#x}")
_, r = streams[5]
for i in range(struct.unpack_from("<I", d, r)[0]):
    start, dsz, drva = struct.unpack_from("<QII", d, r + 4 + 16 * i)
    if start <= rsp < start + dsz:
        st = d[drva + rsp - start: drva + dsz]
        for k in range(0, min(len(st), 0x1000) - 8, 8):
            w = where(struct.unpack_from("<Q", st, k)[0])
            if "DoS" in w or "Archon" in w:
                print(f"  rsp+{k:#x} {w}")
print("modules:", [m[2] for m in mods if "DoS" in m[2] or "Archon" in m[2]])
