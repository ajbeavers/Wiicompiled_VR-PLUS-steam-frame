# Game data and toolchain: developer notes

Nothing disc-derived is ever committed or released. Every step below runs on the user's own Frame
from the user's own disc. Run heavy steps under `flock ~/Games/KartFrame/.build.lock` while the
shared build lock is in use. Timings are from the Frame (5 CPUs), page cache warm.

| Step | Command | Output | Time |
| --- | --- | --- | --- |
| Extract | `frame/setup.sh extract <iso> <dest>` | `<dest>/DATA`, `main.dol`, `StaticR.rel`, `.complete` | 3 s warm (a minute or two cold) |
| Translator publish (maintainer, once) | `frame/publish-translator.sh <outdir>` | `<outdir>/Translator.Cli` (34 MiB, self-contained single file), `.sha256` | not timed alone (10 min wall, mostly waiting for the build lock) |
| Translate | `frame/setup.sh translate <dest> <out>` | `<out>/generated`, `<out>/build/base` | 49 s (translate-recursive 42 s, base manifest 1 s, data init 0.5 s, shards 5 s) |
| Toolchain | `frame/toolchain.sh [destdir]` | `<root>/bin/{clang,clang++,ld.lld,llvm-ar,cmake,ninja}`, 581 MiB | about 75 s plus about 1.8 GB of downloads |
| Boundary check | `frame/boundary-check.sh <dir\|tarball> [allowlist]` | exit 1 on any disc-derived file | seconds |

## Inputs and pins

- Disc: PAL `RMCP01`. `setup.sh` reads the game id with `nodtool info` first, then checks
  `main.dol` and `StaticR.rel` against the sha256 values in `projects/mkwii/recomp.yml`.
- nodtool `v2.0.0-alpha.10` (the version upstream pins) from encounter/nod, linux-aarch64 asset,
  sha256 `b0d94617ed2393334845669cc1f74f1e4fce0d6acb24e8be28c479689a1b907b`; cached in `~/.cache/kartframe/nodtool/<version>/`.
- .NET 8 SDK (arm64) in `~/.local/opt/dotnet`, installed with Microsoft's `dotnet-install.sh
  --channel 8.0`. Only the publish step needs it; set `DOTNET_CLI_TELEMETRY_OPTOUT=1`.
- Toolchain: upstream's `Launcher/prepare-portable-tools.sh --arch aarch64` (patched LLVM 22.1.8,
  CMake 4.3.3, Ninja 1.13.2, all sha256-pinned there). `toolchain.sh` runs a copy of it from
  `~/.cache/kartframe/toolchain/launcher` so downloads and scratch stay out of the repo. The
  downloads can be deleted after the first run (`toolchain/launcher/artifacts`, about 1.8 GB).

## Extracted layout (same as upstream's Linux setup, `DiscTool.cs`)

```
<dest>/main.dol, StaticR.rel   translator inputs (the recomp.yml `Assets/` paths)
<dest>/DATA/sys, DATA/files    nodtool data partition; the runtime's [paths] dvd_root
<dest>/.complete               written last; records game id, nodtool version, pins
```

## Translation

`setup.sh translate` is the base-product sequence of `Launcher/local-build.sh`:
`translate-recursive`, `emit-base-manifest`, `generate-data-init --target-os linux`,
`emit-build-shards`. `<out>` is laid out as a workspace root (`projects`, `Assets`, `runtime` are
links; `generated/` and `build/base/` are produced) because `recomp.yml` paths are
workspace-relative and the shard lists contain absolute paths. Use the same `<out>` when compiling.

Compared with the v1.3.1 translation made by the Windows installer, `generated/` is byte-identical
(base_translation_output.json, sources.bin, symbol table, all 106 shard .cpp and traits, shard map)
except:
1. `data_sections_init_blobs.S`: section directive (`.rdata,"dr"` vs `.rodata,"a",@progbits`).
2. Absolute paths in `data_sections_init_blobs.S` and `build_shards/shards.cmake`; identical
   after substituting the workspace root.
3. An empty `generated/functions/` directory (`--prune-stale` creates it here).

## Compiler check

With the toolchain `bin` first on PATH, `which clang` resolves to it. A C++20 program using
`<format>` builds and runs, and one `base_common` shard compiles (`-std=gnu++17 -O2`, 14 s, 2.2 MB
object) and links with `ld.lld`. The default standard library is the host's libstdc++ and libc
headers (`/usr/include`); `-stdlib=libc++` also needs `-Wl,-rpath,<root>/lib/aarch64-unknown-linux-gnu`
to run. The toolchain has no sysroot, so glibc and linux-api headers must exist on the host.
