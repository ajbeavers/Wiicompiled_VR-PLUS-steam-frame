# Build dependencies for the Steam Frame kit objects

`steam-frame/build.sh` compiles three translation units of the upstream project for Android:
the runtime unity groups `runtime_4` and `runtime_6` (they hold the Frame-patched
`vr/openxr_input.cpp` and `vr/openxr_integration.cpp`, among other sources) and Aurora's
`lib/gfx/common.cpp`. Upstream fetches its third-party dependencies with CMake on Windows; the
Linux build on the Frame does not run CMake at all, so this directory carries exactly the
third-party headers those three units include, and nothing else:

| Directory | Project | Version in upstream's workspace | Licence |
| --- | --- | --- | --- |
| `Dependencies/SDL` | [SDL3](https://github.com/libsdl-org/SDL) | 3.4.4 | zlib (`LICENSE.txt`) |
| `Dependencies/fmt` | [{fmt}](https://github.com/fmtlib/fmt) | 11.1.4 | MIT (`LICENSE`) |
| `Dependencies/imgui` | [Dear ImGui](https://github.com/ocornut/imgui) | 1.91.9b | MIT (`LICENSE.txt`) |
| `Dependencies/openxr` | [OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) | 1.1.61 | Apache 2.0 (`LICENSE`) |
| `Dependencies/abseil-cpp` | [Abseil](https://github.com/abseil/abseil-cpp) | as pinned upstream | Apache 2.0 (`LICENSE`) |
| `Dependencies/tracy` | [Tracy](https://github.com/wolfpld/tracy) | as pinned upstream | BSD 3-Clause (`LICENSE`) |
| `Dependencies/xxhash` | [xxHash](https://github.com/Cyan4973/xxHash) | 0.8.x | BSD 2-Clause (`LICENSE`) |
| `Dependencies/dawn_prebuilt/include` | [Dawn](https://dawn.googlesource.com/dawn) / webgpu-headers | upstream's prebuilt Dawn package | BSD 3-Clause (the licence text heads each header) |

`generated/` holds two kinds of file that CMake would otherwise write:

- `unity/*.cxx`: the runtime's unity groups, each a list of the sources it compiles together.
  They came from a CMake configure of upstream's `runtime/` on this Frame (the group membership
  is the same on every platform), with the checkout path replaced by `@SRC@`, which `build.sh`
  fills in. Only `runtime_4` and `runtime_6` are compiled; the rest document the kit's layout.
- `sdl-build/include-revision/SDL3/SDL_revision.h`: SDL's generated revision header.

Everything else the build needs is in the repository (`runtime/`, `aurora-main/`,
`runtime/third_party/`) or in the Android NDK that `build.sh` downloads.
