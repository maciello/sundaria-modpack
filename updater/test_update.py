"""python3 test_update.py — exercises update.py against file:// fake releases."""
import json, os, subprocess, sys, tempfile, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def release(d, tag, files):
    z = d / f"{tag}.zip"
    with zipfile.ZipFile(z, "w") as zf:
        for n, body in files.items():
            zf.writestr(n, body)
    api = d / f"{tag}.json"
    api.write_text(json.dumps({"tag_name": tag, "assets": [{"name": z.name, "browser_download_url": z.as_uri()}]}))
    return api.as_uri()


def run(api, game):
    env = {**os.environ, "MODPACK_API": api}
    cmd = [sys.executable, str(HERE / "update.py"), "true", "waitforexitandrun", str(game / "Archon/Binaries/Win64/Archon-Win64-Shipping.exe")]
    return subprocess.run(cmd, env=env, capture_output=True, text=True)


with tempfile.TemporaryDirectory() as t:
    d, game = Path(t), Path(t) / "game"
    game.mkdir()
    w = "Archon/Binaries/Win64/"
    v1 = release(d, "v1", {w + "winmm.dll": "a", w + "old.asi": "1"})
    v2 = release(d, "v2", {w + "winmm.dll": "b", w + "new.asi": "2"})
    evil = release(d, "evil", {"../escape.txt": "x"})

    r = run(v1, game); assert r.returncode == 0 and "installed v1" in r.stderr, r
    assert (game / w / "old.asi").read_text() == "1"
    r = run(v1, game); assert "up to date" in r.stderr, r
    r = run(v2, game); assert "installed v2" in r.stderr, r
    assert not (game / w / "old.asi").exists() and (game / w / "winmm.dll").read_text() == "b"
    r = run("file:///nonexistent.json", game); assert r.returncode == 0 and "skipped" in r.stderr, r
    r = run(evil, game); assert "bad path" in r.stderr and not (d / "escape.txt").exists(), r
    assert (game / ".modpack-version").read_text() == "v2"
print("ok")
