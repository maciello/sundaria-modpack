#!/usr/bin/env python3
"""Ask the running game (dev install, live-bridge feature) over 127.0.0.1:47811. Skill ref: references/live-bridge.md.
  get <path> [depth]               reflected value: pawn.mAbilitySystemComponent.SpawnedAttributes[1]
  find <class> [near <m>]          actors nearest first, each with an @i root for get/call
  call <path> <Function> [json]    UFunction on the game thread, scalar args: '[1.5, "Fire", true]' (host only)
  trace <regex> [seconds]          UFunctions through ProcessEvent matching Class::Function, counted
  shot [path] [x y w h]            PNG of the current frame (default: <Win64>/dos-tool-shots/<ms>.png)
  log [n]                          last n lines of dos-tool.log
  ping
Output: YAML (JSON without PyYAML). Exit 1 on an error answer. Env: GAME_WIN64 (shot paths), GAME_BRIDGE_PORT."""
import json
import os
import socket
import sys

PORT = int(os.environ.get("GAME_BRIDGE_PORT", "47811"))


def ask(line, timeout, port=PORT):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as s:
        s.settimeout(timeout)
        s.sendall(line.encode() + b"\n")
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(1 << 16)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf)


def to_windows(path):
    """Host path -> the game's view (Proton maps Z: to /)."""
    return "Z:" + os.path.abspath(path).replace("/", "\\")


def to_host(path, win64):
    """The game's Windows path -> host path."""
    if path[:3].upper() == "Z:\\":
        return path[2:].replace("\\", "/")
    mark = "\\Binaries\\Win64\\"
    if win64 and mark in path:
        return os.path.join(win64, path.split(mark, 1)[1].replace("\\", "/"))
    return path


def line_for(args):
    if args and args[0] == "shot" and len(args) in (2, 6):  # shot <path> [x y w h]: a host path
        args = [args[0], to_windows(args[1]), *args[2:]]
    return " ".join(args)


def timeout_for(args):
    if args and args[0] == "trace":
        try:
            return min(float(args[2]), 60) + 10 if len(args) > 2 else 15
        except ValueError:
            return 15
    return 10


def dump(x):
    try:
        import yaml
        return yaml.safe_dump(x, sort_keys=False, allow_unicode=True, width=120).rstrip()
    except ImportError:
        return json.dumps(x, indent=1)


def main(args):
    if not args or args[0] in ("-h", "--help", "help"):
        print(__doc__)
        return 0
    try:
        r = ask(line_for(args), timeout_for(args))
    except ConnectionRefusedError:
        print(f"!! nothing on 127.0.0.1:{PORT}: game not running, no dos-tool.dev, or Live bridge off (Insert menu)", file=sys.stderr)
        return 1
    except (socket.timeout, TimeoutError):
        print("!! no answer in time", file=sys.stderr)
        return 1
    if not r.get("ok"):
        print(dump({"error": r.get("error")}))
        return 1
    res = r["result"]
    if args[0] == "shot" and isinstance(res, dict):
        res["path"] = to_host(res["path"], os.environ.get("GAME_WIN64", ""))
    print(dump(res))
    return 0


def self_test():
    import threading
    assert to_host("Z:\\tmp\\a.png", "") == "/tmp/a.png"
    assert to_host("S:\\x\\Archon\\Binaries\\Win64\\dos-tool-shots\\1.png", "/g/Win64") == "/g/Win64/dos-tool-shots/1.png"
    assert to_windows("/tmp/x.png") == "Z:\\tmp\\x.png"
    assert line_for(["shot", "/tmp/a.png", "0", "0", "10", "10"]) == "shot Z:\\tmp\\a.png 0 0 10 10"
    assert line_for(["get", "pawn.Health"]) == "get pawn.Health"
    assert timeout_for(["trace", "Hit", "30"]) == 40
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]

    def serve():
        c, _ = srv.accept()
        got = c.recv(100)
        assert got == b"get pawn\n", got
        c.sendall(b'{"ok":true,"result":{"$class":"X"}}\n')
        c.close()
    t = threading.Thread(target=serve)
    t.start()
    assert ask("get pawn", 5, port) == {"ok": True, "result": {"$class": "X"}}
    t.join()
    srv.close()
    print("game.py self-test: ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test()
    else:
        sys.exit(main(sys.argv[1:]))
