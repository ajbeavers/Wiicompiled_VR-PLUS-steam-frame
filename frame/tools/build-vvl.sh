#!/bin/bash
# Build the Khronos validation layer (VK_LAYER_KHRONOS_validation) from source and install it
# for the current user: ~/.local/share/vulkan/explicit_layer.d (JSON + .so). There is no
# package for the Frame. Takes the machine build lock (same one as kfbuild.sh), nice, -j4.
#   tools/build-vvl.sh [vulkan-sdk tag, default vulkan-sdk-1.4.309.0]
# Use:  VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation <app>   (or VK_LOADER_LAYERS_ENABLE)
# Pinned dependency commits are the ones in the tag's scripts/known_good.json.
set -e
TAG=${1:-vulkan-sdk-1.4.309.0}
W=${VVL_WORK:-$HOME/vvl-build}
OUT=$HOME/.local/share/vulkan/explicit_layer.d
exec 9>"$HOME/Games/KartFrame/.build.lock"; echo "build-vvl: waiting for lock"; flock 9
mkdir -p "$W/deps" "$OUT"
fetch() { # name url ref
  if [ ! -d "$W/deps/$1/.git" ]; then mkdir -p "$W/deps/$1"; git -C "$W/deps/$1" init -q; git -C "$W/deps/$1" remote add origin "$2"; fi
  git -C "$W/deps/$1" fetch -q --depth 1 origin "$3" && git -C "$W/deps/$1" checkout -q FETCH_HEAD
}
[ -d "$W/src/.git" ] || git clone -q --depth 1 --branch "$TAG" https://github.com/KhronosGroup/Vulkan-ValidationLayers.git "$W/src"
KG=$W/src/scripts/known_good.json
pin() { python3 -c "import json,sys; d={r['name']:r for r in json.load(open('$KG'))['repos']}; print(d['$1']['commit'])"; }
fetch Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers.git "$(pin Vulkan-Headers)"
fetch Vulkan-Utility-Libraries https://github.com/KhronosGroup/Vulkan-Utility-Libraries.git "$(pin Vulkan-Utility-Libraries)"
fetch SPIRV-Headers https://github.com/KhronosGroup/SPIRV-Headers.git "$(pin SPIRV-Headers)"
fetch SPIRV-Tools https://github.com/KhronosGroup/SPIRV-Tools.git "$(pin SPIRV-Tools)"
fetch robin-hood-hashing https://github.com/martinus/robin-hood-hashing.git "$(pin robin-hood-hashing)"
P=$W/prefix
CM="cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_PREFIX_PATH=$P"
build() { # name extra-args...  (name "layers" builds $W/src)
  n=$1; shift
  s=$W/deps/$n; [ "$n" = layers ] && s=$W/src
  { $CM -S "$s" -B "$W/b-$n" "$@" && nice -n 10 ninja -j4 -C "$W/b-$n" install; } > "$W/b-$n.log" 2>&1 \
    || { tail -30 "$W/b-$n.log" >&2; echo "build of $n failed (log $W/b-$n.log)" >&2; exit 1; }
  echo "built $n"
}
build Vulkan-Headers
build Vulkan-Utility-Libraries
build SPIRV-Headers
build SPIRV-Tools -DSPIRV-Headers_SOURCE_DIR="$W/deps/SPIRV-Headers" -DSPIRV_SKIP_TESTS=ON \
  -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON -DBUILD_SHARED_LIBS=OFF
build robin-hood-hashing -DRH_STANDALONE_PROJECT=OFF
# robin_hood 3.11.5 lacks <cstdint>, which newer libstdc++ no longer pulls in by itself
build layers -DCMAKE_CXX_FLAGS="-include cstdint" -DBUILD_TESTS=OFF -DBUILD_WERROR=OFF -DUSE_ROBIN_HOOD_HASHING=ON
cp "$P/lib/libVkLayer_khronos_validation.so" "$OUT/"
sed 's#"library_path": *"[^"]*"#"library_path": "'"$OUT"'/libVkLayer_khronos_validation.so"#' \
  "$P/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json" > "$OUT/VkLayer_khronos_validation.json"
echo "installed to $OUT"
