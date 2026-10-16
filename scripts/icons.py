#!/usr/bin/env python3
"""Skill icons (#145): one game-style ability icon per entry of the skill list, made by Fooocus on the dump host.

  icons.py --win64 DIR [--list F] [--out D] [--only ID] [--force] [--dry-run]
  icons.py --self-test

List (default <Win64>/dos-mods/skills.yaml): YAML list of flat entries; only `id` (file name) and `icon` (what the icon
shows, English) are read here. Output (default <Win64>/dos-mods/icons/): <id>.png 256x256 + <id>.txt (the prompt). An
existing icon is kept unless --force. Same id = same seed, so a rerun gives the same icon.

Generation: `ssh dumps` runs the gradio_client that ships in Fooocus's own venv (version-matched to its Gradio 3.41)
against 127.0.0.1:7865 there; nothing is installed or changed on that machine. The generate function is found in
/config at run time (the dependency with >100 inputs, then the one chained after it), never by a fixed index.
Env: ICONS_HOST (default dumps), FOOOCUS_PY (default ~/Fooocus/fooocus_env/bin/python).
"""
import argparse
import base64
import json
import os
import subprocess
import sys
import zlib
from pathlib import Path

STYLE = ("painted fantasy RPG ability icon, {hint}, single centered symbol, bold silhouette, rich painted texture, "
         "dramatic rim light, dark vignette background, square game icon, no text")
NEGATIVE = "text, letters, words, numbers, watermark, signature, logo, frame text, ui, photo, blurry, lowres, multiple icons"
MODEL = "DreamShaperXL_Turbo_v2_1.safetensors"
SIZE = 256


def parse_list(text):
    """The subset of YAML the skill list uses: `- key: value` starts an entry, `  key: value` continues it."""
    entries, cur = [], None
    for raw in text.splitlines():
        line = raw.split(" #")[0].rstrip() if not raw.lstrip().startswith("#") else ""
        if not line.strip():
            continue
        item = line.lstrip().startswith("- ")
        body = line.lstrip()[2:] if item else line.strip()
        if item:
            cur = {}
            entries.append(cur)
        if cur is None or ":" not in body:
            continue
        k, v = body.split(":", 1)
        v = v.strip()
        if len(v) >= 2 and v[0] == v[-1] and v[0] in "'\"":
            v = v[1:-1]
        cur[k.strip()] = v
    return entries


def check(entries):
    """Entries usable for icons; problems as messages (missing id/icon, bad id, duplicates)."""
    ok, bad, seen = [], [], set()
    for i, e in enumerate(entries, 1):
        sid = e.get("id", "")
        if not sid or not all(c.isalnum() or c in "_-" for c in sid) or sid != sid.lower():
            bad.append(f"entry {i}: id '{sid}' must be lowercase letters, digits, _ or -")
        elif sid in seen:
            bad.append(f"entry {i}: id '{sid}' twice")
        elif not e.get("icon"):
            bad.append(f"{sid}: no icon hint")
        else:
            seen.add(sid)
            ok.append(e)
    return ok, bad


def seed(sid):
    return zlib.crc32(sid.encode()) & 0x7FFFFFFF


def job(e):
    return {"id": e["id"], "prompt": STYLE.format(hint=e["icon"].strip().rstrip(".")), "negative": NEGATIVE, "seed": seed(e["id"])}


# Runs on the dump host with Fooocus's python: reads JOBS, prints one JSON line per icon {"id", "png"} (base64, SIZE px).
REMOTE = r'''
import base64, io, json, sys
from gradio_client import Client
from PIL import Image
c = Client("http://127.0.0.1:7865/", verbose=False, serialize=False)
comps = {x["id"]: x for x in c.config["components"]}
deps = c.config["dependencies"]
gen = next(i for i, d in enumerate(deps) if len(d["inputs"]) > 100)
run = next(i for i, d in enumerate(deps) if d.get("trigger_after") == gen)
ins = [comps[i] for i in deps[gen]["inputs"]]
labels = [x.get("props", {}).get("label") for x in ins]
def at(label, nth=0):
    return [i for i, l in enumerate(labels) if l == label][nth]
ratios = [a[0] if isinstance(a, list) else a for a in ins[at("Aspect Ratios")]["props"]["choices"]]
for j in JOBS:
    vals = [x.get("props", {}).get("value") for x in ins]
    vals[at(None, 1)] = j["prompt"]  # the prompt box has no label (index 1 of the unlabelled: 0 is the state)
    for label, v in [("Negative Prompt", j["negative"]), ("Selected Styles", []), ("Performance", "Speed"),
                     ("Aspect Ratios", next(a for a in ratios if a.startswith("1024×1024"))), ("Image Number", 1),
                     ("Seed", str(j["seed"])), ("Base Model (SDXL only)", MODEL), ("Guidance Scale", 2.0),
                     ("Sampler", "dpmpp_sde_gpu"), ("Scheduler", "karras"), ("Forced Overwrite of Sampling Step", 8),
                     ("Output Format", "png"), ("Black Out NSFW", False)]:
        vals[at(label)] = v
    vals[at("Enable", 0)] = False  # LoRA 1: the SDXL offset LoRA does not fit the Turbo model
    c.predict(*[v for v, x in zip(vals, ins) if x["type"] != "state"], fn_index=gen)
    out = c.predict(fn_index=run)
    files = [f["name"] for o in out if isinstance(o, dict) and isinstance(o.get("value"), list) for f in o["value"]]
    if not files:
        print(json.dumps({"id": j["id"], "error": "no image in " + json.dumps(out)[:300]}), flush=True)
        continue
    img = Image.open(files[-1]).convert("RGB").resize((SIZE, SIZE), Image.LANCZOS)
    buf = io.BytesIO()
    img.save(buf, "PNG")
    print(json.dumps({"id": j["id"], "png": base64.b64encode(buf.getvalue()).decode()}), flush=True)
'''


def remote_program(jobs):
    return f"JOBS = {json.dumps(jobs)}\nMODEL = {MODEL!r}\nSIZE = {SIZE}\n" + REMOTE


def generate(jobs, out):
    host = os.environ.get("ICONS_HOST", "dumps")
    py = os.environ.get("FOOOCUS_PY", "~/Fooocus/fooocus_env/bin/python")
    p = subprocess.Popen(["ssh", host, py, "-"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8")
    p.stdin.write(remote_program(jobs))
    p.stdin.close()
    prompts, done = {j["id"]: j for j in jobs}, 0
    for line in p.stdout:
        if not line.startswith("{"):
            continue
        r = json.loads(line)
        if "error" in r:
            print(f"!! {r['id']}: {r['error']}")
            continue
        (out / f"{r['id']}.png").write_bytes(base64.b64decode(r["png"]))
        j = prompts[r["id"]]
        (out / f"{r['id']}.txt").write_text(f"{j['prompt']}\nnegative: {j['negative']}\nseed: {j['seed']}\nmodel: {MODEL}\n", encoding="utf-8")
        done += 1
        print(f"icon {r['id']}.png ({done}/{len(jobs)})", flush=True)
    if p.wait() != 0:
        sys.exit(f"!! generation on {host} failed (exit {p.returncode}): is Fooocus running there on 127.0.0.1:7865?")
    return done


def self_test():
    text = """# comment
- id: blink
  name: Blink   # trailing comment
  does: Teleport ein paar Meter, kurzer Lichtschweif
  icon: "glowing white feather, blue light"

- id: double_jump
  icon: two boots leaping upward.
- id: Bad Id
  icon: x
- id: no_icon
  name: x
- id: blink
  icon: again
"""
    e = parse_list(text)
    assert [x.get("id") for x in e] == ["blink", "double_jump", "Bad Id", "no_icon", "blink"], e
    assert e[0]["name"] == "Blink" and e[0]["icon"] == "glowing white feather, blue light", e[0]
    ok, bad = check(e)
    assert [x["id"] for x in ok] == ["blink", "double_jump"], ok
    assert len(bad) == 3 and "Bad Id" in bad[0] and "no_icon" in bad[1] and "twice" in bad[2], bad
    j = job(ok[1])
    assert "two boots leaping upward, single centered" in j["prompt"] and "does" not in j["prompt"], j  # hint without its dot
    assert "Teleport" not in job(ok[0])["prompt"]  # `does` (may be German) never reaches the prompt
    assert j["seed"] == seed("double_jump") == job(ok[1])["seed"] and seed("blink") != seed("double_jump")
    prog = remote_program([j])
    compile(prog, "remote", "exec")
    assert "double_jump" in prog and "from gradio_client import Client" in prog
    print("icons self-test ok")


def main():
    if "--self-test" in sys.argv:
        return self_test()
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--win64", required=True)
    ap.add_argument("--list")
    ap.add_argument("--out")
    ap.add_argument("--only", action="append", help="this id only (repeatable)")
    ap.add_argument("--force", action="store_true", help="replace existing icons")
    ap.add_argument("--dry-run", action="store_true", help="print the prompts, generate nothing")
    a = ap.parse_args()
    mods = Path(a.win64) / "dos-mods"
    lst = Path(a.list) if a.list else mods / "skills.yaml"
    out = Path(a.out) if a.out else mods / "icons"
    if not lst.exists():
        sys.exit(f"!! no skill list at {lst}")
    ok, bad = check(parse_list(lst.read_text(encoding="utf-8")))
    for b in bad:
        print(f"!! {b}")
    if a.only:
        missing = set(a.only) - {e["id"] for e in ok}
        if missing:
            sys.exit(f"!! not in the list: {', '.join(sorted(missing))}")
        ok = [e for e in ok if e["id"] in a.only]
    out.mkdir(parents=True, exist_ok=True)
    todo = [e for e in ok if a.force or not (out / f"{e['id']}.png").exists()]
    print(f"{len(ok)} skill(s), {len(ok) - len(todo)} icon(s) kept, {len(todo)} to make -> {out}")
    jobs = [job(e) for e in todo]
    if a.dry_run:
        for j in jobs:
            print(f"{j['id']} (seed {j['seed']}): {j['prompt']}")
        return
    if jobs:
        generate(jobs, out)


if __name__ == "__main__":
    main()
