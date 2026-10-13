#!/bin/bash
# Builds Aurora's patched Dawn for linux-aarch64 (static, monolithic, Vulkan + Null) and installs it
# as a package that aurora's Dawn provider consumes (AURORA_DAWN_PACKAGE_URL = package directory).
#
#   frame/build-dawn.sh [--force]
#
# Same pinned Dawn revision, patches and CMake flags as android/Build-QuestDawn.ps1, minus the Android
# ones. The package lands in $KARTFRAME_CACHE/dawn/<rev>-<patch hash 12>/ (default
# ~/.cache/kartframe) with aurora-dawn.json, and is reused when the inputs match. Heavy: holds the
# shared build lock (~/Games/KartFrame/.build.lock when present), -j4, nice 10.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CACHE=${KARTFRAME_CACHE:-$HOME/.cache/kartframe}
JOBS=${JOBS:-4}
FORCE=0; [ "${1:-}" = "--force" ] && FORCE=1

REVISION=13abc3bc8ea2d3c2050f9e77a12d012108ceee24
ARCHIVE_SHA256=713bea5b92d4f6c5175752fd7cbf1c3c5ce36598ff5dd98685d8a1216614ebba
PATCH_DIR=$REPO/aurora-main/patches/dawn

FLAGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DDAWN_FETCH_DEPENDENCIES=ON
  -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC
  -DBUILD_SHARED_LIBS=OFF
  -DDAWN_ENABLE_INSTALL=ON
  -DDAWN_BUILD_SAMPLES=OFF
  -DDAWN_BUILD_TESTS=OFF
  -DDAWN_BUILD_BENCHMARKS=OFF
  -DDAWN_USE_GLFW=OFF
  -DDAWN_BUILD_PROTOBUF=OFF
  -DTINT_BUILD_TESTS=OFF
  -DTINT_BUILD_CMD_TOOLS=OFF
  -DTINT_BUILD_IR_BINARY=OFF
  -DDAWN_ENABLE_VULKAN=ON
  -DDAWN_ENABLE_NULL=ON
  -DDAWN_ENABLE_DESKTOP_GL=OFF
  -DDAWN_ENABLE_OPENGLES=OFF
  # The flat mirror window needs a surface: keep the window system backends (headers are in SteamOS).
  -DDAWN_USE_X11=ON
  -DDAWN_USE_WAYLAND=ON
)

# Patch hash, as Build-QuestDawn.ps1: file name + LF-normalised text of every patch input.
PATCH_HASH=$(python3 -I - "$PATCH_DIR" "$REPO" <<'PY'
import hashlib, sys
patch, repo = sys.argv[1], sys.argv[2]
files = [patch + "/apply.py", patch + "/aurora_vulkan_hooks.h", patch + "/aurora_vulkan_interop.inc",
         patch + "/aurora_fdm.h", patch + "/aurora_fdm.inc",
         repo + "/aurora-main/include/aurora/dawn_vulkan_abi.h",
         repo + "/aurora-main/include/aurora/dawn_fdm_abi.h"]
parts = []
for f in files:
    text = open(f, encoding="utf-8", newline="").read().replace("\r\n", "\n")
    parts.append(f.rsplit("/", 1)[1] + "\n" + text)
print(hashlib.sha256("\n".join(parts).encode("utf-8")).hexdigest())
PY
)
CC_ID=$(clang --version | head -1)
CACHE_KEY=$(printf '%s' "$REVISION|$ARCHIVE_SHA256|$PATCH_HASH|$CC_ID|${FLAGS[*]}" | sha256sum | cut -d' ' -f1)
PACKAGE=$CACHE/dawn/$REVISION-${PATCH_HASH:0:12}
LIB=$PACKAGE/lib/libwebgpu_dawn.a

if [ "$FORCE" = 0 ] && [ -f "$PACKAGE/aurora-dawn.json" ] && [ -f "$LIB" ] &&
   grep -q "\"CacheKey\": \"$CACHE_KEY\"" "$PACKAGE/aurora-dawn.json"; then
  echo "Patched Dawn is up to date: $PACKAGE"; exit 0
fi

LOCK=${KARTFRAME_BUILD_LOCK:-$HOME/Games/KartFrame/.build.lock}
mkdir -p "$(dirname "$LOCK")"
exec 9>"$LOCK"; echo "build-dawn: waiting for the build lock"; flock 9; echo "build-dawn: building"

WORK=$CACHE/dawn-work
mkdir -p "$WORK"
ARCHIVE=$WORK/dawn-source.tar.gz
if [ ! -f "$ARCHIVE" ]; then
  curl -fL --retry 3 -o "$ARCHIVE.part" "https://github.com/google/dawn/archive/$REVISION.tar.gz"
  mv "$ARCHIVE.part" "$ARCHIVE"
fi
echo "$ARCHIVE_SHA256  $ARCHIVE" | sha256sum -c - || { echo "Dawn archive does not match the pinned SHA-256" >&2; exit 1; }

# Patches go on pristine sources: src/ is extracted again when the patches changed; third_party/
# (filled by DAWN_FETCH_DEPENDENCIES) is kept.
SRC=$WORK/dawn-$REVISION
MARKER=$SRC/aurora-patches.sha256
if [ ! -f "$MARKER" ] || [ "$(cat "$MARKER")" != "$PATCH_HASH" ]; then
  if [ -d "$SRC" ]; then
    rm -rf "$SRC/src"
    python3 -I -c 'import sys,tarfile
a,d=sys.argv[1:3]
with tarfile.open(a) as t:
    m=[x for x in t.getmembers() if x.name.split("/",2)[1:2]==["src"]]
    t.extractall(d,members=m,filter="data")' "$ARCHIVE" "$WORK"
  else
    python3 -I -c 'import sys,tarfile
a,d=sys.argv[1:3]
with tarfile.open(a) as t: t.extractall(d,filter="data")' "$ARCHIVE" "$WORK"
  fi
  python3 -I "$PATCH_DIR/apply.py" "$SRC"
  echo -n "$PATCH_HASH" > "$MARKER"
fi

BUILD=$WORK/build-$CACHE_KEY
export CCACHE_BASEDIR=$HOME CCACHE_DIR=${CCACHE_DIR:-$HOME/.cache/ccache-kf}
L=(); command -v ccache >/dev/null && L=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld \
  -DPython3_EXECUTABLE="$(command -v python3)" "${L[@]}" "${FLAGS[@]}" -DCMAKE_INSTALL_PREFIX="$PACKAGE"
rm -rf "$PACKAGE"
nice -n 10 cmake --build "$BUILD" --parallel "$JOBS"
cmake --install "$BUILD"
[ -f "$PACKAGE/lib/libwebgpu_dawn.a" ] || { echo "Dawn archive missing" >&2; exit 1; }
command -v llvm-strip >/dev/null && llvm-strip --strip-debug "$PACKAGE/lib/libwebgpu_dawn.a"

LIB_SHA=$(sha256sum "$PACKAGE/lib/libwebgpu_dawn.a" | cut -d' ' -f1)
cat > "$PACKAGE/aurora-dawn.json" <<JSON
{
  "SourceRevision": "$REVISION",
  "SourceSha256": "$ARCHIVE_SHA256",
  "PatchSha256": "$PATCH_HASH",
  "AuroraVulkanAbi": 1,
  "AuroraFdmAbi": 1,
  "Target": "linux-aarch64",
  "Compiler": "$CC_ID",
  "Flags": "${FLAGS[*]}",
  "CacheKey": "$CACHE_KEY",
  "ArchiveSha256": "$LIB_SHA"
}
JSON
echo "Patched Dawn ready: $PACKAGE"
