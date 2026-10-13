#!/usr/bin/env bash
# Maintainer step: build the game kit (everything the first-run build links except the translated
# game) with the SAME portable toolchain and sysroot the user's machine will use, and export it.
#
#   make-kit.sh <kit-outdir>
#
# Environment: KARTFRAME_DAWN_DIR (patched Dawn package; default the newest pinned one),
#   KF_KIT_BUILD (build dir, default <repo>/build-kit), KF_SYSROOT_CACHE (default
#   ~/.cache/kartframe/sysroot), JOBS (default 4). Needs a development translation at <repo>/generated
#   (its shard lists only supply compile flags; none of it ends up in the kit). Takes the shared
#   build lock (~/Games/KartFrame/.build.lock) for the compile.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
[[ $# -eq 1 ]] || { echo "usage: make-kit.sh <kit-outdir>" >&2; exit 2; }
out=$1
build=${KF_KIT_BUILD:-$repo/build-kit}
sysroot=${KF_SYSROOT_CACHE:-$HOME/.cache/kartframe/sysroot}
dawn=${KARTFRAME_DAWN_DIR:-$(ls -dt "$HOME"/.cache/kartframe/dawn/*/ 2>/dev/null | head -1)}
dawn=${dawn%/}
[[ -d $dawn/lib ]] || { echo "no patched Dawn package (frame/build-dawn.sh)" >&2; exit 1; }
[[ -e $repo/generated/build_shards/shards.cmake ]] || { echo "no translation at $repo/generated (flags only; see frame/DEV.md)" >&2; exit 1; }

tc=$("$here/toolchain.sh")
"$here/make-sysroot.sh" "$sysroot"

if [[ ! -f $build/build.ninja ]]; then
    # No --sysroot here: SDL3 and friends need the host's X11/Wayland/ALSA headers. The sysroot is a copy
    # of the host's glibc/libstdc++/kernel headers and crt files (make-sysroot.sh), so the objects built
    # here are the ones the sysroot would have produced; the first-run build compiles and links with it.
    PATH=$tc/bin:$PATH cmake -S "$repo/runtime" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$tc/bin/clang" -DCMAKE_CXX_COMPILER="$tc/bin/clang++" \
        -DCMAKE_AR="$tc/bin/llvm-ar" -DCMAKE_RANLIB="$tc/bin/llvm-ranlib" \
        -DCMAKE_MAKE_PROGRAM="$tc/bin/ninja" \
        -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" \
        -DMKW_DAWN_PACKAGE_DIR="$dawn" -DMKW_BUILD_TESTS=OFF
fi
exec 9>"$HOME/Games/KartFrame/.build.lock"
echo "make-kit: waiting for the build lock"; flock 9
"$tc/bin/ninja" -C "$build" -j"${JOBS:-4}" kartframe_kit_probe
"$here/export-kit.py" --build "$build" --sysroot "$sysroot" --out "$out"
