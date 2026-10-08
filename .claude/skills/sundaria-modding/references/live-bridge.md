# Live bridge (`just game`): ask the running game

Runtime state and visuals, in seconds, with no probe, rebuild or play session. Feature `features/live-bridge/`
(Alpha), host CLI `scripts/game.py`. Offline logic is `just data` (`game-data.md`); layouts are `sdk.py`.
Ask the bridge first. Write a probe only when it can't answer (see "Can't" below).

```bash
just game ping                                        # bridge up? listener registered?
just game get pawn                                    # every property of the hero (depth 1)
just game get pawn.mAbilitySystemComponent.SpawnedAttributes[1] 0   # an attribute set; trailing number = depth 0..4
just game get cam.DefaultFOV
just game find Character near 30                      # actors within 30 m, nearest first, @i handles
just game get @0.mAbilitySystemComponent              # a find result as a root
just game call @0 K2_GetActorLocation                 # UFunction, game thread, host only
just game call pc ClientMessage '["hi"]'              # scalar args as a JSON array
just game trace 'OnProjectileHit|Montage' 10          # UFunctions through ProcessEvent for 10 s, counted
just game shot                                        # PNG of the current frame -> prints its host path
just game shot /tmp/hud.png 0 0 640 360               # host path + crop x y w h
just game log 60                                      # last 60 lines of dos-tool.log
```

```yaml
needs: dev install (`dos-tool.dev` next to the exe), the Live bridge feature on (default with the dev flag), game in a level
roots: {pc: local player controller, pawn: pc.Pawn, ps: pc.PlayerState, cam: pc.PlayerCameraManager, hud: pc.MyHUD,
        world: UWorld, gi: world.OwningGameInstance, gs: world.GameState, "@i": i-th actor of the last find,
        "obj:<Name>": any object by short name (one GObjects walk per command)}
path: "root.Prop.Prop[i].Prop: object pointers (also weak/soft), structs, TArray elements and fixed arrays are followed"
names: case-insensitive; a user-defined struct field `Name_12_<GUID>` answers to `Name`; a miss lists what exists
output: YAML; objects as {$class, $name, <every property>}; pointers as "Class Name"; structs/arrays expand per depth
  (arrays show the first 64, then "+N more"); maps/sets show {num}; enums show the enumerator name
call: host/standalone only (refused on a co-op client); args bool, ints, float, double, enum (number or enumerator
  name), string; struct/object/name args unsupported; prints return + out params; runs the function for real
trace: pattern = regex subset (a|b, ^, $, ., x*), case-insensitive, matched against "DeclaringClass::Function";
  counts per function x object class with first/last second; game thread only (worker-thread calls not counted);
  at most 60 s; script-to-script calls that skip ProcessEvent never show (cast-indicator #81: OnProjectileHit)
shot: back buffer after our overlay drew (what the player sees); default file <Win64>/dos-tool-shots/<ms>.png;
  host paths are passed as Z:\... (Proton maps Z: to /); formats RGBA8, BGRA8, RGB10A2 (others: error with the format)
```

## How it works
```yaml
transport: "TCP 127.0.0.1:47811 (live_bridge::kPort), one command line in, one JSON line out:
  {ok: true, result: ...} | {ok: false, error: ...}. Bound to loopback only; no auth beyond loopback + the dev flag:
  any local process can talk to it while dos-tool.dev exists"
reachable_from_linux: "Proton + pressure-vessel keep the host network namespace: the game process's /proc/<pid>/ns/net
  equals the host shell's, and the game's own loopback LISTEN sockets show in the host's `ss -tlnp`. A Wine named pipe
  would not be reachable from Linux; that is why it is TCP"
threads:
  bridge: "own thread, select() 250 ms; socket, log, shot (PNG encode + write), trace window"
  game: "get/find/call: posted as one job, run by our ProcessEvent listener on the next world tick
    (umg::IsWorldTick); 3 s without a world tick = error (main menu, loading screen)"
  render: "OnFrame starts the thread once and (un)registers the listener; shot copies the back buffer in the
    overlay::SetPresentTap tap (one frame stall per shot)"
idle_cost: "no client = thread asleep in select(), no ProcessEvent listener; listener registered while a client is
  connected and 10 s after its last command (kLingerMs)"
unload: "Off() stops the thread (sockets closed, waits up to 5 s), clears the tap, unregisters the listener;
  log `[live-bridge] closed`"
reflection: core/reflect.* (Find, Walk, Json, ObjectJson, Set); the probes use the same walk
```

## Can't (write a probe instead)
- Watch a value over time or react to an event with logic: a feature-local listener (trace only counts).
- Anything before a world exists (main menu has no world tick: get/find/call time out; log, shot, trace work).
- Calls with struct/object args, writing properties.
