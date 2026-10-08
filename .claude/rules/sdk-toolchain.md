# SDK and toolchain (not in repo)
- Shared dump store on the dump host (ssh alias `dumps`): `/srv/dumps/<game>/<steam-buildid>/` + `dump.yaml`. Sundaria: `/srv/dumps/sundaria/<buildid>/CppSDK`.
- Steam build id: `steamapps/appmanifest_587520.acf` → `buildid`. SDK must match the installed game's build id.
- `just build` uses `/srv/dumps/sundaria/<buildid>/CppSDK` and `/srv/toolchains/xwin-msvc` when they exist on this machine, else `../sdk/CppSDK` and `../tools/msvc`. `SDK_DIR` / `XWIN` override.
- Other machines: `just build` runs `just sdk-pull` first if SDK or MSVC kit is missing (copies to `../sdk/CppSDK`, `../tools/msvc`).
- Build needs `clang` (clang-cl), `lld`, `llvm` (llvm-lib, llvm-rc, llvm-mt), `cmake`, `just`. Fedora: `sudo dnf install clang lld llvm cmake just`.
- Game patch → re-dump SDK with Dumper-7 → push to the dump store under the new build id → rebuild → release.
