# Nintendo Switch build

The Switch release build uses devkitA64/libnx, a pinned devkitPro SDL3 revision, a pinned PPSSPP FFmpeg revision, `-O3`, LTO, and 16 parallel jobs by default. It produces:

- `dist/PPSSPP.nro`: the complete emulator with the standard PPSSPP frontend.
- `dist/PPSSPP-build.txt`: source, dependency, compiler, and SHA-256 metadata.

The Mesa 26.2.2 SDK is not stored in this repository. Extract the user-provided SDK outside the source tree. If it is installed at `/opt/devkitpro/portlibs/switch`, run:

```sh
bash scripts/build-switch.sh
```

For any other location, point to the directory containing the SDK's `include`, `lib`, and `share` directories:

```sh
SWITCH_MESA_PREFIX=/path/to/mesa-sdk/opt/devkitpro/portlibs/switch bash scripts/build-switch.sh
```

The script checks the Mesa version, revision, and packaged checksums before building. SDL and FFmpeg sources are fetched from their public repositories at fixed revisions and built into ignored local directories. Use `bash scripts/build-switch.sh clean` for a full dependency and application rebuild.

The Mesa SDK is only read from its installed prefix. No Mesa or Vulkan SDK files are copied into the repository or release directory.

Required devkitPro components are devkitA64, libnx, switch-tools, CMake, Ninja, Make, Git, and the Switch portlibs required by PPSSPP and the Mesa SDK. Run the script from a devkitPro MSYS2 shell on Windows. Override the job count with `JOBS=n` if necessary.

## Installation

Copy the NRO to:

```text
sdmc:/switch/ppsspp-nx/PPSSPP.nro
```

PPSSPP configuration and Memory Stick data are stored below `sdmc:/switch/ppsspp-nx/PSP`.

Lossless Scaling frame generation is optional, Vulkan-only, and never bundled. To enable it, supply your own compatible library at:

```text
sdmc:/switch/ppsspp-nx/lsfg/Lossless.dll
```
