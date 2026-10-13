# Game kit, first-run setup and toolchain: developer notes

Nothing disc-derived is ever committed, put in the kit, or released. What ships is a **kit**; the
game itself is built on the user's Frame from the user's own disc. Timings are from the Frame
(5 CPUs, 15 GB, page cache warm).

## What a user runs

`frame/setup.sh <iso>` (the app dir's `setup.sh`), once, idempotent and resumable. It needs only the
stock system tools (bash, coreutils, awk, sed, grep, sha256sum, curl for nodtool) plus what ships
with the release: `kit/`, `toolchain/`, `translator/`. No system compiler, cmake, ninja or python is used.

| Step | What | Time (scratch run, 5 CPUs) |
| --- | --- | --- |
| verify kit | `kit.json` fingerprint recomputed, every kit file checked against `kit.sha256` | under 1 s |
| extract | nodtool checks RMCP01 and the `main.dol` / `StaticR.rel` pins, extracts to `$KARTFRAME_HOME/game` | 2 s warm (a minute or two cold) |
| translate | translator -> `$KARTFRAME_HOME/build/{generated,build}` | 49 s |
| build | `kitbuild.cmake` writes `build/kit-build/build.ninja`; the toolchain's ninja compiles 72 translated + 17 product shards, 2 data sources, 1 assembly (3 parallel) and links `kartframe` | 305 s (PCH 2x, shards, link 14 s) |
| install | `bin/kartframe`, resources beside it, `kartframe.fingerprint`, `kartframe.build.txt`, `portable.txt`, `UserData/Config.toml` if missing | under 1 s |
| **total** | | **356 s** (about 6 minutes; the 20 minute budget has room for slower disks and a cold extract) |

A second run does nothing (0.3 s). Deleting objects or the binary and rerunning rebuilds only those
(ninja); the relinked binary is byte-identical (sha256 equal), the build is reproducible.
`setup.sh status` shows what is done. `--force` redoes everything.

Layout (`LANE_RULES.md` "Install layout"): `$KARTFRAME_HOME` (default `~/.local/share/KartFrame`)
holds `portable.txt`, `UserData/`, `game/`, `build/`, `bin/`. `portable.txt` is found from the
executable's directory (`bin/`) one level up, so the game's `ApplicationDataDirectory()` is
`$KARTFRAME_HOME/UserData` (Config.toml, NAND, Cache, Logs; checked in the acceptance run: the
game wrote there and `~/.local/share/WiiCompiled` was not created). `Config.toml` is seeded only if
missing: from `Config.frame.toml` (token `@DVD_ROOT@` -> `$KARTFRAME_HOME/game/DATA`), else just
`[paths] dvd_root`.

Environment: `KARTFRAME_HOME`, `KARTFRAME_KIT` (default `<app>/kit`), `KARTFRAME_TOOLCHAIN`
(default `<app>/toolchain`, then the cache, then `toolchain.sh` makes it), `KARTFRAME_TRANSLATOR`,
`KARTFRAME_CACHE`, `KARTFRAME_JOBS` (default 3, fewer if RAM is short: about 2.5 GB per compile),
`KARTFRAME_THREADS`.

The launch wrapper compares `<app>/kit/kit.json` `"fingerprint"` with `bin/kartframe.fingerprint`
(plain fingerprint string) and refuses to start on a mismatch; rerunning setup rebuilds against the
new kit (the translation is redone too, because it depends on the kit's runtime sources).

## The kit (`frame/make-kit.sh <dir>` then `frame/export-kit.py`, maintainer only)

`make-kit.sh` configures `build-kit` with the portable toolchain, builds the target
`kartframe_kit_probe` (WiiCompiled without any translated code, `runtime/cmake/PublicProducts.cmake`),
and runs `export-kit.py`, which reads everything from the real build graph (`ninja -t commands`,
`-t compdb`) instead of hand-written flags:

```
kit/kit.json           compile flags per source kind (translated shards, product/registration shards,
                       runtime data sources, blob assembly), source slots (which translator list fills
                       which slot), link recipe with {kit}/{sysroot}/{workspace}, fingerprint
kit/kitbuild.cmake     the first-run build: writes build.ninja from kit.json + the translator's shard lists
kit/objects, libs      prebuilt runtime objects; aurora, Dawn (patched, static), SDL3, OpenXR loader,
                       abseil, zlib, libpng, freetype, fmt, imgui, zstd, sqlite, tracy ...: all static
kit/include            runtime/include (everything the translated code includes: only this, checked)
kit/sysroot            see below
kit/workspace          projects/mkwii/{recomp.yml,MAP.txt} and runtime/src: the translator reads them
                       (native overrides), so the translation always matches the kit's runtime
kit/resources          dsp_coef.bin, initial_pipeline_cache.db, wii_bootstrap/, resources/quest_touch_plus/:
                       copied beside the executable (taken from the product's own POST_BUILD copies)
kit/licenses           repo and third-party licence texts
kit/kit.sha256         sha256 of every other file; fingerprint = sha256(sha256(kit.json without its
                       fingerprint line) + sha256(kit.sha256))
```

Resources, none disc-derived: `dsp_coef.bin` (4 KiB DSP coefficient ROM, already in the repo, hash
pinned in CMake), `initial_pipeline_cache.db` (shader recipe database), `wii_bootstrap/` (first-run
WiiConnect24 stubs), Touch Plus models (MIT, WebXR input profiles).

`export-kit.py` fails when a link input is outside the build or the Dawn package, is not a static
archive, or a `-l` library is outside the glibc stub set; it then compiles one sample of every
source kind with exactly the recorded flags and the kit's sysroot and fails if a header outside the
kit, sysroot, compiler resource dir or the sample's own directory is read.
`export-kit.py` needs python3 (maintainer machine only). Compile decisions worth knowing:
`-mcpu=native` is kept as in the build (all Frames are the same SoC; it is resolved on the user's
machine); PCHs of `mkw_pch.h` are made at setup (a PCH only works with the compiler that wrote it);
the translated slot links as `--start-lib ... --end-lib`, like the Quest kit.

Sizes: kit 184 MiB unpacked (libs 74, objects 18, resources 40, sysroot 55), 23.8 MiB as `.tar.xz`;
toolchain 581 MiB unpacked, 113 MiB as `.tar.xz`; sysroot 55 MiB (2306 files, included in the kit).

## Self-contained runtime: DT_NEEDED of `kartframe`

Rule: packages installed after the OS base image (`filesystem`, `glibc` and most others: 2026-09-15;
the owner's hand-installed dev packages: 2026-10-07) are not stock SteamOS. `pacman -Qo` / `-Qi`:

| DT_NEEDED (dev build, lane 0) | Package | Installed | Decision |
| --- | --- | --- | --- |
| libpng16.so.16 | libpng 1.6.43 | 10-07, explicit | static: aurora's pinned libpng 1.6.58 from source |
| libabsl_*.so.2508 (38 libs) | abseil-cpp 20250814 | 10-07 | static: aurora's pinned abseil 20240722.0 from source |
| libfreetype.so.6 | freetype2 2.13.2 | 10-07 | static: pinned freetype 2.14.3, harfbuzz / brotli / bzip2 / png off |
| (via freetype) libharfbuzz, libgraphite2 | harfbuzz 8.4.0, graphite | 10-07 | gone with the static freetype |
| (via freetype) libglib-2.0, libpcre2-8, libbrotli*, libbz2 | glib2, pcre2, brotli, bzip2 | 09-15 | gone (stock anyway) |
| libz.so.1 | zlib 1.3.1 | 09-15, stock | static anyway (libpng needs it; zlib 1.3.2 pinned source) |
| libm.so.6, libc.so.6, ld-linux-aarch64.so.1 | glibc 2.39 | 09-15, stock | dynamic |
| libstdc++.so.6, libgcc_s.so.1 | gcc-libs 15.1.1 | 09-15, stock | dynamic |

The switch is `AURORA_VENDOR_THIRD_PARTY` (aurora-main/extern/CMakeLists.txt), on by default on
Linux. Result, `readelf -d` / `ldd` of the binary built by `setup.sh`: `libm`, `libstdc++`,
`libgcc_s`, `libc`, `ld-linux-aarch64` only. Libraries the program loads at run time with `dlopen`
(SDL3's ALSA / PulseAudio / PipeWire / JACK / X11 / Wayland / DRM drivers, libdbus for the music
attenuation, libvulkan through Dawn) are not link dependencies; their package dates here are
10-07 (`alsa-lib`, `libx11`, `wayland`, `vulkan-headers` ...), which looks like owner installs or
upgrades. They are optional for SDL (it falls back) and Valve's own SteamVR needs libvulkan, X11 or
Wayland; not verified on a second Frame.

## Toolchain and sysroot

Toolchain: upstream's `Launcher/prepare-portable-tools.sh --arch aarch64` through `toolchain.sh`
(patched LLVM 22.1.8: clang, clang++, ld.lld, llvm-ar, llvm-ranlib; CMake 4.3.3; Ninja 1.13.2; all
sha256-pinned there). Fetched once (about 1.8 GB of downloads, 75 s) or shipped as
`toolchain-aarch64.tar.xz`.

The portable LLVM has no sysroot. With no `--sysroot` it reads the host's `/usr/include`, its libstdc++
headers (`/usr/include/c++/15.1.1`), `crt*.o`, libgcc and libs. Same package/date method:

| Used by the first-run build | Package | Installed | Stock? |
| --- | --- | --- | --- |
| glibc headers, `Scrt1.o crti.o crtn.o`, `libc.so`, `libm.so`, `libc_nonshared.a` | glibc 2.39 | 09-15 | yes |
| `libstdc++.so`, `libgcc_s.so` | gcc-libs 15.1.1 | 09-15 | yes |
| libstdc++ headers `/usr/include/c++/15.1.1` | gcc 15.1.1 | 10-07 | **no** (dev package) |
| `crtbeginS.o crtendS.o libgcc.a libgcc_eh.a` | gcc 15.1.1 | 10-07 | **no** |
| kernel headers (`linux/`, `asm/`, ...) | linux-api-headers 6.18 | 10-07 | **no** (dated after the image) |

So the kit carries a minimal sysroot (`frame/make-sysroot.sh`, 55 MiB, 2306 files) built from this
Frame's own packages, unmodified: glibc headers and crt objects, libc/libm/libstdc++/libgcc_s and
the dynamic loader (link-time copies, the binary still loads the user's own), libstdc++ headers,
kernel headers, `libgcc.a`, `libgcc_eh.a`, `crtbegin*/crtend*`. Licences are in
`sysroot/LICENSES` (glibc LGPL-2.1+, GCC GPL-3 with the Runtime Library Exception, kernel headers
GPL-2 with the syscall note). Every compile and the link use `--sysroot={sysroot}`; checked: the
include search list and `-H` output of a shard compile, and `-Wl,-t` of the link, name only files
under the kit sysroot, the toolchain's own resource dir (`stddef.h` and friends), and the kit.
Objects of the kit itself were built without `--sysroot` (SDL3 needs the host's X11/Wayland/ALSA
headers at configure time); the sysroot is a copy of the same files, so they are what it would produce.

## Extraction, translation (unchanged from phase 1)

- Disc: PAL `RMCP01`; `setup.sh` reads the game id with `nodtool info`, then checks `main.dol` and
  `StaticR.rel` against the sha256 pins in `recomp.yml`. nodtool `v2.0.0-alpha.10` (the version
  upstream pins), linux-aarch64 asset, sha256 `b0d94617ed2393334845669cc1f74f1e4fce0d6acb24e8be28c479689a1b907b`,
  cached in `~/.cache/kartframe/nodtool/<version>/`.
- Extracted layout (as upstream's `DiscTool.cs`): `main.dol`, `StaticR.rel` (translator inputs),
  `DATA/` (the runtime's `[paths] dvd_root`), `.complete` (written last).
- Translator: `frame/publish-translator.sh <outdir>` (maintainer, once, .NET 8 SDK in
  `~/.local/opt/dotnet`) makes a 34 MiB self-contained single-file `Translator.Cli`.
- `setup.sh translate` is the base-product sequence of `Launcher/local-build.sh`: `translate-recursive`,
  `emit-base-manifest`, `generate-data-init --target-os linux`, `emit-build-shards`. `<outdir>` is a
  workspace root (`projects`, `Assets`, `runtime` are links; here `projects` and `runtime` point at the
  kit's `workspace/`, in a source tree at the repo). Compared with the Windows installer's translation,
  `generated/` is byte-identical except the blob assembly's section directive and absolute paths.
- Boundary check: `frame/boundary-check.sh <dir|tarball> [allowlist]` fails on disc-derived file types,
  translator output names and directories, and files over 1 MB that `frame/kit-allowlist.txt` (globs)
  does not allow. `frame/tools/privacy-check.sh <dir|tarball>` needs these entries in
  `frame/tools/privacy-allow.txt` for the kit's third-party headers and licence texts (false positives:
  author addresses in glibc and kernel headers, example IPs): `^(kit/)?sysroot/usr/include/`,
  `^(kit/)?licenses/third-party/`, `^(kit/)?workspace/runtime/src/hle/net/network_socket\.cpp$`, each with
  text regex `.*`. With them: 0 findings on the kit dir and the tarball.
