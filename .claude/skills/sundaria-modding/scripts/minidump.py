#!/usr/bin/env python3
"""minidump.py [dmp] [--dll DoS-Tool.dll] — fault module+offset, registers, stack return addresses of a UE crash;
DoS-Tool frames as function file:line (llvm-symbolizer + the build's PDB).
Default dmp: newest UE4Minidump.dmp in the game's Proton prefix. DLL: the one of build/ or the game's Win64 whose
link timestamp + image size equal the crashed module's (PDB next to it); --dll forces one (e.g. a rebuild whose .text
matches). Stdlib only (pip `minidump` chokes on UE's custom streams)."""
import glob, json, os, pathlib, shutil, struct, subprocess, sys

args = sys.argv[1:]
force = args.pop(args.index("--dll") + 1) if "--dll" in args else None
args = [a for a in args if a != "--dll"]
dmp = args[0] if args else max(glob.glob(os.path.expanduser(
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
    base, size, _, stamp, nr = struct.unpack_from("<QIIII", d, o)
    mods.append((base, base + size, d[nr + 4:nr + 4 + struct.unpack_from("<I", d, nr)[0]].decode("utf-16le").split("\\")[-1], stamp))
mod = lambda a: next(((b, nm, ts, e - b) for b, e, nm, ts in mods if b <= a < e), None)
where = lambda a: (lambda m: f"{m[1]}+{a - m[0]:#x}" if m else "?")(mod(a))


def stamp_size(dll):  # PE header: TimeDateStamp, SizeOfImage
    b = open(dll, "rb").read(0x400)
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    return struct.unpack_from("<I", b, pe + 8)[0], struct.unpack_from("<I", b, pe + 24 + 56)[0]


def find_dll(ts, size):
    if force:
        return force
    repo = pathlib.Path(__file__).resolve().parents[4]
    win64 = os.environ.get("GAME_WIN64", os.path.expanduser("~/.steam/steam/steamapps/common/DungeonsofSundaria/Archon/Binaries/Win64"))
    for dll in (repo / "mods/dos-tool/build/DoS-Tool.dll", pathlib.Path(win64) / "DoS-Tool.dll"):
        if dll.exists() and stamp_size(dll) == (ts, size) and dll.with_suffix(".pdb").exists():
            return str(dll)


short = lambda f: next((f.split(k)[-1] for k in ("/mods/dos-tool/", "/CppSDK/", "/include/") if k in f), f)


def symbolize(frames):  # frames: [(label, addr, is_return)] → label  module+off  func file:line < inliner …
    sym = shutil.which("llvm-symbolizer")
    first = next((mod(a) for _, a, _ in frames if mod(a) and "DoS-Tool" in mod(a)[1]), None)
    dll = first and find_dll(first[2], first[3])
    names = {}
    if sym and dll:
        ours = [a for _, a, _ in frames if mod(a) and mod(a)[0] == first[0]]
        offs = {a: a - first[0] - (1 if ret else 0) for _, a, ret in frames if a in ours}  # return address → its call
        out = subprocess.run([sym, f"--obj={dll}", "--relative-address", "--output-style=JSON"], input="\n".join(hex(o) for o in offs.values()),
                             capture_output=True, text=True).stdout
        for a, res in zip(offs, (json.loads(l) for l in out.splitlines() if l.strip())):
            chain = [f"{s['FunctionName']} {short(s['FileName'])}:{s['Line']}" for s in res.get("Symbol", []) if s.get("Line")]  # Line 0 = data
            if chain:
                names[a] = "  " + " < ".join(chain)
    print(f"symbols: {dll or 'none'}" + ("" if dll else " (no build DLL with the crashed timestamp+size and a PDB next to it; --dll <rebuild>)"))
    for label, a, _ in frames:
        if a in names or "DoS" not in where(a):
            print(f"{label} {where(a)}{names.get(a, '')}")
        elif not dll:
            print(f"{label} {where(a)}")


_, r = streams[6]
code, _, _, addr = struct.unpack_from("<IIQQ", d, r + 8)
ctx = struct.unpack_from("<II", d, r + 8 + 152)[1]
reg = lambda off: struct.unpack_from("<Q", d, ctx + off)[0]
rsp = reg(0x98)
print(dmp)
print(f"code {code:#x}  rcx {reg(0x80):#x} rdx {reg(0x88):#x} r8 {reg(0xB8):#x}")
frames = [("fault", addr, False)]
_, r = streams[5]
for i in range(struct.unpack_from("<I", d, r)[0]):
    start, dsz, drva = struct.unpack_from("<QII", d, r + 4 + 16 * i)
    if start <= rsp < start + dsz:
        st = d[drva + rsp - start: drva + dsz]
        for k in range(0, min(len(st), 0x1000) - 8, 8):
            a = struct.unpack_from("<Q", st, k)[0]
            if "DoS" in where(a) or "Archon" in where(a):
                frames.append((f"  rsp+{k:#x}", a, True))
symbolize(frames)  # stack scan: unsymbolized DoS-Tool hits (data pointers) are dropped once a PDB is found
print("modules:", [m[2] for m in mods if "DoS" in m[2] or "Archon" in m[2]])
