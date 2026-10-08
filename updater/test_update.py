"""python3 test_update.py — exercises update.py (and update.ps1 if PWSH=/path/to/pwsh) against a local fake GitHub."""
import functools, http.server, json, os, subprocess, sys, tempfile, threading, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
UPDATERS = {"py": [sys.executable, str(HERE / "update.py")]}
if os.environ.get("PWSH"):
    UPDATERS["ps1"] = [os.environ["PWSH"], "-NoProfile", "-File", str(HERE / "update.ps1")]


def check(updater, d, base):
    def release(tag, files):
        with zipfile.ZipFile(d / f"{tag}.zip", "w") as zf:
            for n, body in files.items():
                zf.writestr(n, body)
        (d / f"{tag}.json").write_text(json.dumps(
            {"tag_name": tag, "assets": [{"name": f"{tag}.zip", "browser_download_url": f"{base}/{tag}.zip"}]}))
        return f"{base}/{tag}.json"

    game = d / "game"
    game.mkdir()

    def run(api):
        env = {**os.environ, "MODPACK_API": api}
        exe = str(game / "Archon/Binaries/Win64/Archon-Win64-Shipping.exe")
        r = subprocess.run(updater + ["true", "waitforexitandrun", exe], env=env, capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr

    w = "Archon/Binaries/Win64/"
    v1 = release("v1", {w + "winmm.dll": "a", w + "old.asi": "1"})
    v2 = release("v2", {w + "winmm.dll": "b", w + "new.asi": "2"})
    evil = release("evil", {"../escape.txt": "x"})

    rc, out = run(v1); assert rc == 0 and (game / w / "old.asi").read_text() == "1", out
    mtime = (game / w / "old.asi").stat().st_mtime_ns
    rc, out = run(v1); assert (game / w / "old.asi").stat().st_mtime_ns == mtime, out  # no re-download
    rc, out = run(v2); assert not (game / w / "old.asi").exists() and (game / w / "winmm.dll").read_text() == "b", out
    rc, out = run(f"{base}/missing.json"); assert rc == 0 and "skipped" in out, out
    rc, out = run(evil); assert "bad path" in out and not (d / "escape.txt").exists(), out
    assert (game / ".modpack-version").read_text().strip() == "v2"


for name, updater in UPDATERS.items():
    with tempfile.TemporaryDirectory() as t:
        d = Path(t)
        handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=t)
        handler.func.log_message = lambda *a: None
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            check(updater, d, f"http://127.0.0.1:{srv.server_port}")
        finally:
            srv.shutdown()
    print(name, "ok")
