# DoS-Tool

A lightweight in-game **camera enhancement overlay** for **Dungeons of Sundaria**
(Unreal Engine 4.27). It adds an ImGui menu to adjust the third-person camera —
built against a [Dumper-7](https://github.com/Encryqed/Dumper-7)-generated SDK and
**cross-compiled on Linux** (clang → MSVC ABI) for use under Windows and Steam/Proton.

> Scope: a personal quality-of-life tool. It only changes **your own camera** (FOV,
> zoom distance, collision) — no gameplay/stat changes, nothing that affects other
> players in co-op. There is no anti-cheat in the game.

<img width="2080" height="1050" alt="image" src="https://github.com/user-attachments/assets/8bbf8a04-0951-4d19-a675-a4806e0fdf3a" />

<img width="2114" height="1027" alt="image" src="https://github.com/user-attachments/assets/9166e688-07c9-4ebb-b1ad-b8e06f669751" />


## Features
- **FOV override** — set the field of view (40–130).
- **Camera distance override** — pull the camera further out / in than the game allows.
- **Disable camera collision** — stop the camera snapping inward at walls (pairs well
  with a larger distance).
- Native ImGui overlay, toggled with a key; reads/writes the live UE objects by exact
  offset via the generated SDK.

## Controls
- **`INSERT`** — toggle the menu.

## Compatibility
Built and tested against **Dungeons of Sundaria** — Steam **build `15427624`**
(≈ 2026-06-30 update), Unreal Engine **4.27.2** (CL 49798, `Archon`).

The tool reads the game's memory at **fixed offsets baked in at build time**, so a
game patch that shifts those class layouts can stop it working (usually a hang or the
menu not appearing). If a future update breaks it, it needs a fresh Dumper-7 dump +
rebuild (see *Build from source*) and a new release — end users just grab the updated
build. You can check your installed build id under *Steam → right-click Dungeons of
Sundaria → Properties → Updates*, or in `steamapps/appmanifest_587520.acf`.

---

## Install

Installed by the modpack updater, see the [modpack README](../../README.md#install-once).

---

## Build from source

See the modpack [README](../../README.md#develop) (`just build`).

## Layout
- `src/` — the tool: `dllmain` (entry + proof log), `game` (SDK access, the **only** TU
  that includes the SDK), `overlay` (kiero D3D11 present hook + ImGui menu).
- `third_party/` — vendored Dear ImGui, kiero, MinHook.
- `vendor/` — Ultimate ASI Loader (`winmm.dll`) for packaging.
- the generated CppSDK is external (see above), referenced by path.

## How it works
`game.cpp` walks `UWorld::GetWorld() → OwningGameInstance → LocalPlayers[0] →
PlayerController → PlayerCameraManager`, verifies the manager is the gameplay
`ABP_PlayerCamera_C` with `IsA` (menus use a different class), then reads/writes
`DefaultFOV`, `kInitialOrbitDistance`, and `ArchonSpringArm->bDoCollisionTest`. The
overlay hooks `IDXGISwapChain::Present` (D3D11, via kiero) and renders ImGui on top.

## Credits
Dumper-7 (Encryqed) · Dear ImGui (ocornut) · kiero (Rebzzel) · MinHook (TsudaKageyu) ·
Ultimate ASI Loader (ThirteenAG).
