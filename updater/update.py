#!/usr/bin/env python3
"""Steam launch option (Linux/Proton):  python3 /path/to/update.py %command%

Pulls the latest GitHub Release zip (paths relative to the game root) into the
game folder, then starts the game. Any failure -> game starts anyway.
"""
import json, os, sys, tempfile, urllib.request, zipfile
from pathlib import Path

API = os.environ.get("MODPACK_API", "https://api.github.com/repos/maciello/sundaria-modpack/releases/latest")


def game_root(argv):
    for a in argv:  # Steam's %command% ends in .../<root>/Archon/Binaries/Win64/Archon-Win64-Shipping.exe
        if a.lower().endswith("archon-win64-shipping.exe"):
            return Path(a).parents[3]
    return None


def sync(root):
    with urllib.request.urlopen(API, timeout=10) as r:
        rel = json.load(r)
    tag = rel["tag_name"]
    ver = root / ".modpack-version"
    if ver.exists() and ver.read_text() == tag:
        return f"up to date ({tag})"
    url = next(a["browser_download_url"] for a in rel["assets"] if a["name"].endswith(".zip"))
    with tempfile.TemporaryFile() as tmp:
        with urllib.request.urlopen(url, timeout=60) as r:
            tmp.write(r.read())
        with zipfile.ZipFile(tmp) as z:
            names = [n for n in z.namelist() if not n.endswith("/")]
            for n in names:  # trust boundary: refuse paths escaping the game root
                if not (root / n).resolve().is_relative_to(root.resolve()):
                    raise ValueError(f"bad path in zip: {n}")
            listing = root / ".modpack-files"
            if listing.exists():
                for old in listing.read_text().splitlines():
                    (root / old).unlink(missing_ok=True)
            z.extractall(root, names)
    listing.write_text("\n".join(names))
    ver.write_text(tag)
    return f"installed {tag}"


def main(argv):
    root = game_root(argv)
    if root:
        try:
            print("[modpack]", sync(root), file=sys.stderr)
        except Exception as e:
            print("[modpack] update skipped:", e, file=sys.stderr)
    if argv:
        os.execvp(argv[0], argv)


if __name__ == "__main__":
    main(sys.argv[1:])
