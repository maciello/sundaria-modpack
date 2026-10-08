# SDK and toolchain (not in repo)
- Shared dump store on the dump host (ssh alias `dumps`): `/srv/dumps/<game>/<steam-buildid>/` + `dump.yaml`. Sundaria: `/srv/dumps/sundaria/<buildid>/CppSDK`.
- Steam build id: `steamapps/appmanifest_587520.acf` → `buildid`. SDK must match the installed game's build id.
- `just sdk-pull` copies the matching SDK to `../sdk/CppSDK`. On the dump host itself: `SDK_DIR=/srv/dumps/sundaria/<buildid>/CppSDK`.
- MSVC kit: `/srv/toolchains/xwin-msvc` on the dump host; set `XWIN=` to it, or rsync it to `../tools/msvc`.
- Build needs `clang` (clang-cl), `lld`, `llvm` (llvm-lib, llvm-rc, llvm-mt), `cmake`, `just`. Fedora: `sudo dnf install clang lld llvm cmake just`.
- Game patch → re-dump SDK with Dumper-7 → push to the dump store under the new build id → rebuild → release.
