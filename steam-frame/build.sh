#!/bin/bash
# Builds the Steam Frame APK of WiiCompiled VR from the stock Quest APK, with a self-contained
# toolchain and no root. Runs on the Frame itself (arm64 Linux); an x86_64 Linux PC should work
# too but has not been tried.
#
#   steam-frame/build.sh [upstream.apk]
#
# Without an argument the stock v1.3.1 Quest APK is downloaded from the upstream release page and
# checked against its published SHA-256. Output: steam-frame/release/WiiCompiled-VR-SteamFrame-<tag>.apk
#
# What it does:
#   1. an Android NDK into ~/.local/opt/android-build (WCF_TOOLS changes the folder);
#   2. compiles the two runtime unity groups that hold the Frame-patched sources (openxr_input.cpp,
#      openxr_integration.cpp, and the rest of each group) and Aurora's gfx/common.cpp, with the
#      same flags the kit's own objects were built with;
#   3. checks that each object exports exactly the symbols of the one it replaces in the upstream kit;
#   4. make-apk.py puts them in the APK's game kit, patches classes.dex, aligns and signs the APK.
# The game itself is never built here: the headset builds it from your own disc against this kit.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); SRC=$(cd "$HERE/.." && pwd)
DEPS=$HERE/build-deps; OUT=$HERE/out
TOOLS=${WCF_TOOLS:-$HOME/.local/opt/android-build}
SIGNING=${WCF_SIGNING:-$HERE/signing}   # key.pem + cert.der; made on first use, keep them for updates to install
UPSTREAM_APK_URL=https://github.com/heurazy/Wiicompiled_VR-PLUS/releases/download/v1.3.1/WiiCompiled-VR-Quest3-v1.3.1.apk
UPSTREAM_APK_SHA256=dc409a4be612b40570682d04b2923b137abeb59895298281d5f56907db321f16

fetch() { [ -s "$2" ] || { echo "downloading $(basename "$2")"; curl -L --retry 3 -o "$2" "$1"; }; }
mkdir -p "$TOOLS/dl" "$OUT" "$HERE/release" "$HERE/dl"

# 0. The stock APK: the kit archive that gets one object replaced comes out of it, and so does
#    the APK everything else is copied from.
APK=${1:-$HERE/dl/WiiCompiled-VR-Quest3-v1.3.1.apk}
[ -s "$APK" ] || fetch "$UPSTREAM_APK_URL" "$APK"
if ! echo "$UPSTREAM_APK_SHA256  $APK" | sha256sum -c --quiet 2>/dev/null; then
    echo "warning: $APK is not the upstream v1.3.1 APK this build was made for; carrying on, the symbol check below decides"
fi

# 1. Toolchain: NDK r29, the one the kit itself was built with and the one the headset links with.
#    A newer NDK's libc++ headers emit helpers (std::__hash_memory in r30) that r29's libc++ does
#    not have, and the on-device link then fails. Google ships no arm64 Linux NDK; HomuHomu833's
#    build of r29 fills in on the Frame.
ARCH=$(uname -m)
if [ -n "${WCF_NDK:-}" ]; then
    NDK=$WCF_NDK
elif [ "$ARCH" = aarch64 ]; then
    NDK=$TOOLS/ndk/android-ndk-r29
    if [ ! -d "$NDK/toolchains/llvm/prebuilt" ]; then
        fetch https://github.com/HomuHomu833/android-ndk-custom/releases/download/r29/android-ndk-r29-aarch64-linux-gnu.tar.xz "$TOOLS/dl/ndk-r29-aarch64.tar.xz"
        mkdir -p "$TOOLS/ndk" && tar xJf "$TOOLS/dl/ndk-r29-aarch64.tar.xz" -C "$TOOLS/ndk"
    fi
elif [ "$ARCH" = x86_64 ]; then
    NDK=$TOOLS/ndk/android-ndk-r29
    if [ ! -d "$NDK/toolchains/llvm/prebuilt" ]; then
        fetch https://dl.google.com/android/repository/android-ndk-r29-linux.zip "$TOOLS/dl/android-ndk-r29-linux.zip"
        mkdir -p "$TOOLS/ndk" && unzip -q "$TOOLS/dl/android-ndk-r29-linux.zip" -d "$TOOLS/ndk"
    fi
else
    echo "unsupported host $ARCH"; exit 1
fi
LLVM=$(ls -d "$NDK"/toolchains/llvm/prebuilt/linux-* | head -1)
CXX=$LLVM/bin/clang++; AR=$LLVM/bin/llvm-ar; NM=$LLVM/bin/llvm-nm
echo "using $("$CXX" --version | head -1)"

# 2. Compile. Flags as in the kit's recipe (assets/game_kit/kit.json) and upstream's Android build.
TARGET=(--target=aarch64-none-linux-android29 --sysroot="$LLVM/sysroot")
COMMON=(-DANDROID -fdata-sections -ffunction-sections -funwind-tables -fstack-protector-strong
    -no-canonical-prefixes -D_FORTIFY_SOURCE=2 -Wformat -Werror=format-security -O3 -DNDEBUG -fPIC
    -std=gnu++20 -O3 -ffast-math -w -pipe -mcpu=cortex-a77 -pthread)
DEFS=(-DAURORA -DAURORA_ENABLE_GX -DIMGUI_ENABLE_FREETYPE '-DIMGUI_USER_CONFIG="aurora/imgui_config.h"'
    -DNOMINMAX -DTARGET_PC -DWEBGPU_DAWN)
RUNTIME_DEFS=("${DEFS[@]}" -DCRYPTOPP_DISABLE_ASM -DCRYPTOPP_NO_GLOBAL_BYTE -DMKW_ENABLE_OPENXR=1
    -D_DISABLE_STRING_ANNOTATION -D_DISABLE_VECTOR_ANNOTATION)
# Third-party headers: only the ones these translation units include, vendored in build-deps.
DEP=$DEPS/Dependencies; GEN=$DEPS/generated
RUNTIME_INC=(-I"$SRC/runtime/include" -I"$SRC/runtime/src" -I"$SRC" -I"$SRC/aurora-main/include"
    -I"$DEP/fmt/include" -I"$GEN/sdl-build/include-revision" -I"$DEP/SDL/include" -I"$DEP/xxhash" -I"$DEP/imgui"
    -I"$SRC/runtime/third_party/pugixml" -I"$DEP/openxr/include"
    -I"$SRC/runtime/third_party/libco" -I"$DEP/dawn_prebuilt/include" -I"$DEP/vulkan_headers/include"
    -isystem "$SRC/runtime/third_party/toml11" -isystem "$SRC/runtime/third_party")
AURORA_INC=(-I"$SRC/aurora-main/include" -I"$DEP/fmt/include" -I"$GEN/sdl-build/include-revision" -I"$DEP/SDL/include"
    -I"$DEP/xxhash" -I"$DEP/imgui" -I"$DEP/sqlite3" -isystem "$DEP/dawn_prebuilt/include" -isystem "$DEP/tracy/public"
    -isystem "$DEP/abseil-cpp")

rm -rf "$OUT/obj" "$OUT/kit"; mkdir -p "$OUT/obj" "$OUT/kit/objects" "$OUT/kit/libs" "$OUT/kit/include/vr"
# The unity groups are CMake's: generated/unity lists which runtime sources each one holds.
for entry in 007:runtime_4 001:runtime_6; do
    n=${entry%%:*}; g=${entry#*:}
    sed "s|@SRC@/|$SRC/|g" "$GEN/unity/unity_${g}_cxx.cxx" > "$OUT/obj/unity_${g}_cxx.cxx"
    "$CXX" "${TARGET[@]}" "${RUNTIME_DEFS[@]}" "${RUNTIME_INC[@]}" "${COMMON[@]}" -include mkw_pch.h \
        -c "$OUT/obj/unity_${g}_cxx.cxx" -o "$OUT/kit/objects/${n}_unity_${g}_cxx.cxx.o" &
done
# fmt's format.h uses malloc without including <cstdlib> itself; a newer libc++ no longer does it for it.
"$CXX" "${TARGET[@]}" "${DEFS[@]}" "${AURORA_INC[@]}" "${COMMON[@]}" -include cstdlib \
    -c "$SRC/aurora-main/lib/gfx/common.cpp" -o "$OUT/obj/common.cpp.o" &
wait
echo "compiled"

# 3. Symbol check against the kit in the APK, then the Aurora archive with its one member swapped.
rm -rf "$OUT/upstream-kit"; mkdir -p "$OUT/upstream-kit"
unzip -q -o "$APK" 'assets/game_kit/objects/007_unity_runtime_4_cxx.cxx.o' 'assets/game_kit/objects/001_unity_runtime_6_cxx.cxx.o' \
    'assets/game_kit/libs/020_libaurora_gx.a' 'assets/game_kit/include/vr/openxr_input.h' -d "$OUT/upstream-kit"
UK=$OUT/upstream-kit/assets/game_kit
exported() { "$NM" --defined-only "$1" | awk '$2 ~ /[TDBR]/ {print $3}' | sort -u; }
imported() { "$NM" -u "$1" | awk '{print $2}' | sort -u; }
ok=1
# Same exports as the kit's object, and no new C++ imports: a new mangled import means a different
# libc++ than the kit's, which the on-device link cannot satisfy. New plain C imports (bionic) are fine.
check() {  # label kit-object new-object
    if diff <(exported "$2") <(exported "$3") >/dev/null; then
        echo "$1: exports the kit's symbols"
    else
        echo "$1: exported symbols DIFFER from the kit's (sources out of step with this APK?)"; ok=0
    fi
    new_cxx=$(comm -13 <(imported "$2") <(imported "$3") | grep '^_Z' || true)
    if [ -n "$new_cxx" ]; then
        echo "$1: imports C++ symbols the kit's object does not (wrong NDK/libc++?):"; echo "$new_cxx" | head -5; ok=0
    fi
}
for o in 007_unity_runtime_4_cxx.cxx.o 001_unity_runtime_6_cxx.cxx.o; do
    check "$o" "$UK/objects/$o" "$OUT/kit/objects/$o"
done
t=$(mktemp -d); (cd "$t" && "$AR" x "$UK/libs/020_libaurora_gx.a" common.cpp.o)
check "020_libaurora_gx.a(common.cpp.o)" "$t/common.cpp.o" "$OUT/obj/common.cpp.o"
rm -rf "$t"
[ $ok = 1 ] || { echo "not making an APK from objects that do not match the kit"; exit 1; }
cp "$UK/libs/020_libaurora_gx.a" "$OUT/kit/libs/020_libaurora_gx.a"
(cd "$OUT/obj" && "$AR" r "$OUT/kit/libs/020_libaurora_gx.a" common.cpp.o)
# The kit's headers are CRLF (exported on Windows); the changed one goes in the same way.
sed 's/\r$//; s/$/\r/' "$SRC/runtime/include/vr/openxr_input.h" > "$OUT/kit/include/vr/openxr_input.h"

# 4. The APK.
TAG=$(git -C "$SRC" describe --tags --always --dirty 2>/dev/null || echo build)
python3 "$HERE/make-apk.py" build --upstream "$APK" --kit "$OUT/kit" --signing "$SIGNING" \
    --out "$HERE/release/WiiCompiled-VR-SteamFrame-$TAG.apk"
