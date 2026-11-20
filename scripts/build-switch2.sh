#!/usr/bin/env bash
# Builds PPSSPP for the Nintendo Switch 2 (kaiHEN homebrew): dist/PPSSPP-NX2.nro.
#
#   SWITCH2_MESA_SDK=<mesa-26.2.7-switch2-horizon-sdk.zip> bash scripts/build-switch2.sh [clean]
#
# Needs ounce-devkit in $DEVKITPRO (https://github.com/ouncebrew/ounce-devkit) and Mesa 26.2.7's Switch 2 SDK
# (SWITCH2_MESA_SDK, or SWITCH2_MESA_PREFIX for an extracted prefix; default: the mesa-nx checkout next to this one).
# SDL3 (with SDL/switch2.patch) and FFmpeg are rebuilt for the Cortex-A78C in build-switch2-deps.

set -euo pipefail

export PATH="/usr/bin:/bin:${PATH:-}"

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)"
DEPS_DIR="$ROOT_DIR/build-switch2-deps"
BUILD_DIR="$ROOT_DIR/build-switch2-release"
DIST_DIR="$ROOT_DIR/dist"
SDL_SOURCE_DIR="$DEPS_DIR/sdl-src"
SDL_BUILD_DIR="$DEPS_DIR/sdl-build"
SDL_PREFIX="$DEPS_DIR/sdl-prefix"
FFMPEG_SOURCE_DIR="$DEPS_DIR/ffmpeg-src"
FFMPEG_BUILD_DIR="$DEPS_DIR/ffmpeg-build"
FFMPEG_PREFIX="$DEPS_DIR/ffmpeg-prefix"

SDL_REPOSITORY="https://github.com/devkitPro/SDL.git"
SDL_REVISION="c329016c0e338ab4397ad930f83330c9fe058348"
SDL_RECIPE_VERSION="switch2-1"
FFMPEG_REPOSITORY="https://github.com/hrydgard/ppsspp-ffmpeg.git"
FFMPEG_REVISION="1e3b4965632f60b1d85360261d1b9dd45444bc71"
FFMPEG_RECIPE_VERSION="switch2-1"
MESA_VERSION="26.2.7"
# mesa-nx's build-switch2.sh (dist/mesa-26.2.7-switch2-horizon-sdk.zip), with uam2rt.
MESA_SDK_SHA256="e5cf87483ff242b1082e27af5a49fadc71279c55a5f0663986b8b4d76d404971"

remove_build_dirs() {
	local target resolved
	for target in "$@"; do
		resolved="$(realpath -m -- "$target")"
		case "$resolved" in
			"$DEPS_DIR"|"$DEPS_DIR/"*|"$BUILD_DIR") ;;
			*) echo "Refusing to remove directory outside this build: $resolved" >&2; exit 1 ;;
		esac
		[[ ! -L "$target" ]] || { echo "Refusing to remove symlink: $target" >&2; exit 1; }
	done
	rm -rf -- "$@"
}

if [[ "${1:-}" == "clean" ]]; then
	remove_build_dirs "$DEPS_DIR" "$BUILD_DIR"
elif [[ $# -ne 0 ]]; then
	echo "Usage: $0 [clean]" >&2
	exit 2
fi

: "${DEVKITPRO:=/opt/devkitpro}"
export DEVKITPRO
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
# portlibs/switch2/bin first: its aarch64-none-elf-pkg-config is the Switch 2 toolchain's.
export PATH="$DEVKITPRO/portlibs/switch2/bin:$DEVKITPRO/portlibs/switch/bin:$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"
export LC_ALL=C
export TZ=UTC
export ZERO_AR_DATE=1

TOOLCHAIN="$DEVKITPRO/libounce/cmake/Switch2.cmake"
[[ -f "$TOOLCHAIN" ]] || { echo "No Switch 2 devkit in $DEVKITPRO: install https://github.com/ouncebrew/ounce-devkit" >&2; exit 1; }
[[ -f "$DEVKITPRO/libounce/lib/libdrako3d.a" ]] || { echo "drako3d is not installed in $DEVKITPRO/libounce" >&2; exit 1; }
[[ -f "$DEVKITPRO/portlibs/switch2/lib/libkaihen.a" ]] || { echo "libkaihen (kaiHEN's JIT provider) is not installed in portlibs/switch2" >&2; exit 1; }
SWITCH2_CPU="$(sed -n 's/^SWITCH2_CPU[[:space:]]*:=[[:space:]]*//p' "$DEVKITPRO/libounce/switch2_cpu.mk")"
[[ -n "$SWITCH2_CPU" ]] || { echo "No SWITCH2_CPU in $DEVKITPRO/libounce/switch2_cpu.mk" >&2; exit 1; }

# The Switch 2 Mesa SDK.
MESA_SDK_DIR="$DEPS_DIR/mesa-$MESA_VERSION-switch2-sdk"
MESA_PREFIX="${SWITCH2_MESA_PREFIX:-$MESA_SDK_DIR/opt/devkitpro/portlibs/switch2}"
if [[ -z "${SWITCH2_MESA_PREFIX:-}" ]]; then
	SWITCH2_MESA_SDK="${SWITCH2_MESA_SDK:-$ROOT_DIR/../mesa-nx/dist/mesa-$MESA_VERSION-switch2-horizon-sdk.zip}"
	if [[ ! -f "$MESA_SDK_DIR/.ppsspp-sdk-sha256" ]] || [[ "$(<"$MESA_SDK_DIR/.ppsspp-sdk-sha256")" != "$MESA_SDK_SHA256" ]]; then
		[[ -f "$SWITCH2_MESA_SDK" ]] || { echo "Set SWITCH2_MESA_SDK to mesa-$MESA_VERSION-switch2-horizon-sdk.zip" >&2; exit 1; }
		remove_build_dirs "$MESA_SDK_DIR"
		"${PYTHON:-python3}" "$ROOT_DIR/scripts/unpack-switch-mesa.py" "$SWITCH2_MESA_SDK" "$MESA_SDK_DIR" "$MESA_SDK_SHA256" \
			"mesa-$MESA_VERSION-switch2-horizon-sdk.zip"
	fi
fi
[[ -f "$MESA_PREFIX/lib/cmake/Vulkan/VulkanConfig.cmake" ]] || { echo "No Switch 2 Mesa SDK in $MESA_PREFIX" >&2; exit 1; }
grep -Fq "Vulkan_VERSION \"$MESA_VERSION\"" "$MESA_PREFIX/lib/cmake/Vulkan/VulkanConfig.cmake" || { echo "Mesa SDK $MESA_VERSION is required" >&2; exit 1; }
grep -Fq "Switch 2" "$MESA_PREFIX/lib/cmake/Vulkan/VulkanConfig.cmake" || { echo "$MESA_PREFIX is not the Switch 2 Mesa SDK" >&2; exit 1; }
[[ -f "$MESA_PREFIX/lib/cmake/uam2rt/uam2rtConfig.cmake" ]] || { echo "No uam2rt in $MESA_PREFIX" >&2; exit 1; }

JOBS="${JOBS:-16}"
SWITCH_RELEASE_VERSION="${PPSSPP_SWITCH_RELEASE_VERSION:-1.1.2}"

case "$JOBS" in
	''|*[!0-9]*) echo "JOBS must be a positive integer" >&2; exit 2 ;;
	0) echo "JOBS must be greater than zero" >&2; exit 2 ;;
esac
case "$SWITCH_RELEASE_VERSION" in
	''|*[!0-9A-Za-z.+-]*) echo "PPSSPP_SWITCH_RELEASE_VERSION contains invalid characters" >&2; exit 2 ;;
esac

for tool in cmake git make ninja sha256sum tar realpath aarch64-none-elf-gcc aarch64-none-elf-pkg-config; do
	command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done

mkdir -p "$DEPS_DIR" "$DIST_DIR"

SDL_PATCH="$ROOT_DIR/SDL/switch-vulkan-window.patch"
SDL_PATCH2="$ROOT_DIR/SDL/switch2.patch"
SDL_PATCH_SUM="$(cat "$SDL_PATCH" "$SDL_PATCH2" | sha256sum | cut -d' ' -f1)"
SDL_STAMP="$SDL_RECIPE_VERSION $SDL_REVISION $SDL_PATCH_SUM $MESA_SDK_SHA256 $SWITCH2_CPU"
if [[ ! -f "$SDL_PREFIX/.ppsspp-switch2-sdl" ]] || [[ "$(<"$SDL_PREFIX/.ppsspp-switch2-sdl")" != "$SDL_STAMP" ]]; then
	remove_build_dirs "$SDL_SOURCE_DIR" "$SDL_BUILD_DIR" "$SDL_PREFIX"
	git clone --no-checkout "$SDL_REPOSITORY" "$SDL_SOURCE_DIR"
	git -C "$SDL_SOURCE_DIR" checkout --detach "$SDL_REVISION"
	git -C "$SDL_SOURCE_DIR" apply "$SDL_PATCH"
	git -C "$SDL_SOURCE_DIR" apply "$SDL_PATCH2"
	cmake -S "$SDL_SOURCE_DIR" -B "$SDL_BUILD_DIR" -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" \
		-DCMAKE_PREFIX_PATH="$MESA_PREFIX" \
		-DOpenGL_DIR="$MESA_PREFIX/lib/cmake/OpenGL" \
		-DVulkan_DIR="$MESA_PREFIX/lib/cmake/Vulkan" \
		-DSDL_SHARED=OFF \
		-DSDL_STATIC=ON \
		-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON \
		-DSDL_TESTS=OFF \
		-DSDL_EXAMPLES=OFF \
		-DSDL_INSTALL=ON \
		-DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=ON \
		-DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -g0 -ffile-prefix-map=$ROOT_DIR=. -fmacro-prefix-map=$ROOT_DIR=. -DSDL_VIDEO_DRIVER_SWITCH=1 -DSDL_AUDIO_DRIVER_SWITCH=1 -DSDL_JOYSTICK_SWITCH=1 -I$MESA_PREFIX/include"
	cmake --build "$SDL_BUILD_DIR" --target install --parallel "$JOBS"
	printf '%s\n' "$SDL_STAMP" > "$SDL_PREFIX/.ppsspp-switch2-sdl"
fi

FFMPEG_STAMP="$FFMPEG_RECIPE_VERSION $FFMPEG_REVISION $SWITCH2_CPU"
if [[ ! -f "$FFMPEG_PREFIX/.ppsspp-switch2-ffmpeg" ]] || [[ "$(<"$FFMPEG_PREFIX/.ppsspp-switch2-ffmpeg")" != "$FFMPEG_STAMP" ]]; then
	remove_build_dirs "$FFMPEG_SOURCE_DIR" "$FFMPEG_BUILD_DIR" "$FFMPEG_PREFIX"
	mkdir -p "$FFMPEG_SOURCE_DIR" "$FFMPEG_BUILD_DIR" "$FFMPEG_PREFIX"
	FFMPEG_GIT_DIR="$DEPS_DIR/ffmpeg-git"
	if [[ ! -d "$FFMPEG_GIT_DIR" ]]; then
		git init --bare "$FFMPEG_GIT_DIR"
	fi
	if ! git --git-dir="$FFMPEG_GIT_DIR" cat-file -e "$FFMPEG_REVISION^{commit}" 2>/dev/null; then
		git --git-dir="$FFMPEG_GIT_DIR" fetch --depth 1 "$FFMPEG_REPOSITORY" "$FFMPEG_REVISION"
	fi
	git --git-dir="$FFMPEG_GIT_DIR" archive "$FFMPEG_REVISION" | tar -xf - -C "$FFMPEG_SOURCE_DIR"
	FFMPEG_FLAGS="-D__SWITCH__ -D__SWITCH2__ -D_GNU_SOURCE -O3 -DNDEBUG -g0 -flto=auto -fno-fat-lto-objects -ffile-prefix-map=$ROOT_DIR=. -fmacro-prefix-map=$ROOT_DIR=. -isystem $DEVKITPRO/libounce/include -I$MESA_PREFIX/include $SWITCH2_CPU -mtp=soft -fPIE -pie -ffunction-sections -fdata-sections -ftls-model=local-exec"
	(
		cd "$FFMPEG_BUILD_DIR"
		"$FFMPEG_SOURCE_DIR/configure" \
			--prefix="$FFMPEG_PREFIX" \
			--disable-debug \
			--enable-cross-compile \
			--arch=aarch64 \
			--cross-prefix=aarch64-none-elf- \
			--ar=aarch64-none-elf-gcc-ar \
			--nm=aarch64-none-elf-gcc-nm \
			--ranlib=aarch64-none-elf-gcc-ranlib \
			--target-os=horizon \
			--extra-cflags="$FFMPEG_FLAGS" \
			--extra-cxxflags="$FFMPEG_FLAGS" \
			--extra-ldflags="-g0 -flto=auto -fPIE -pie -Wl,--build-id=none -L$MESA_PREFIX/lib -L$DEVKITPRO/libounce/lib" \
			--disable-shared \
			--enable-static \
			--disable-iconv \
			--disable-runtime-cpudetect \
			--disable-everything \
			--enable-zlib \
			--disable-filters \
			--disable-programs \
			--disable-network \
			--disable-avfilter \
			--disable-postproc \
			--disable-encoders \
			--disable-protocols \
			--disable-hwaccels \
			--disable-doc \
			--enable-decoder=h264 \
			--enable-decoder=mpeg4 \
			--enable-decoder=mpeg2video \
			--enable-decoder=mjpeg \
			--enable-decoder=mjpegb \
			--enable-decoder=aac \
			--enable-decoder=aac_latm \
			--enable-decoder=atrac3 \
			--enable-decoder=atrac3p \
			--enable-decoder=mp3 \
			--enable-decoder=pcm_s16le \
			--enable-decoder=pcm_s8 \
			--enable-encoder=huffyuv \
			--enable-encoder=ffv1 \
			--enable-encoder=mjpeg \
			--enable-encoder=pcm_s16le \
			--enable-demuxer=h264 \
			--enable-demuxer=m4v \
			--enable-demuxer=mpegvideo \
			--enable-demuxer=mpegps \
			--enable-demuxer=mp3 \
			--enable-demuxer=avi \
			--enable-demuxer=aac \
			--enable-demuxer=pmp \
			--enable-demuxer=oma \
			--enable-demuxer=pcm_s16le \
			--enable-demuxer=pcm_s8 \
			--enable-demuxer=wav \
			--enable-muxer=avi \
			--enable-parser=h264 \
			--enable-parser=mpeg4video \
			--enable-parser=mpegaudio \
			--enable-parser=mpegvideo \
			--enable-parser=aac \
			--enable-parser=aac_latm
		make -j"$JOBS"
		make install
	)
	printf '%s\n' "$FFMPEG_STAMP" > "$FFMPEG_PREFIX/.ppsspp-switch2-ffmpeg"
fi

if git -C "$ROOT_DIR" rev-parse --verify HEAD >/dev/null 2>&1; then
	export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT_DIR" show -s --format=%ct HEAD)}"
fi

# (no HOME menu forwarder payload: kaiHEN starts homebrew only from its loader)
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
	-U'OPENGL_*' -U'_OPENGL_*' -U'Vulkan_*' -U'_VULKAN_SWITCH_*' \
	-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_PREFIX_PATH="$MESA_PREFIX;$SDL_PREFIX" \
	-DSWITCH2_MESA_PREFIX="$MESA_PREFIX" \
	-DOpenGL_DIR="$MESA_PREFIX/lib/cmake/OpenGL" \
	-DVulkan_DIR="$MESA_PREFIX/lib/cmake/Vulkan" \
	-Duam2rt_DIR="$MESA_PREFIX/lib/cmake/uam2rt" \
	-DSDL3_DIR="$SDL_PREFIX/lib/cmake/SDL3" \
	-DFFMPEG_DIR="$FFMPEG_PREFIX" \
	-DPPSSPP_SWITCH_RELEASE_VERSION="$SWITCH_RELEASE_VERSION" \
	-DUSE_LIBNX=ON \
	-DSWITCH_ENABLE_LTO=ON \
	-DSWITCH_LTO_JOBS="$JOBS" \
	-DUSE_FFMPEG=ON \
	-DUSE_SYSTEM_FFMPEG=OFF \
	-DBUILD_BUNDLED_FFMPEG=OFF \
	-DUSE_DISCORD=OFF \
	-DUSE_MINIUPNPC=OFF \
	-DHEADLESS=OFF \
	-DUNITTEST=OFF \
	-DATLAS_TOOL=OFF

cmake --build "$BUILD_DIR" --target PPSSPP_NRO --parallel "$JOBS"
cmake -E copy_if_different "$BUILD_DIR/PPSSPP-NX2.nro" "$DIST_DIR/PPSSPP-NX2.nro"

SOURCE_REVISION="$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
SOURCE_STATE="clean"
if ! git -c core.autocrlf=input -C "$ROOT_DIR" diff --quiet --ignore-submodules HEAD -- 2>/dev/null || [[ -n "$(git -C "$ROOT_DIR" ls-files --others --exclude-standard)" ]]; then
	SOURCE_STATE="dirty"
fi
NRO_SHA256="$(sha256sum "$DIST_DIR/PPSSPP-NX2.nro" | cut -d' ' -f1)"
{
	echo "PPSSPP Switch 2 $SWITCH_RELEASE_VERSION"
	echo "source=$SOURCE_REVISION"
	echo "source_state=$SOURCE_STATE"
	echo "devkit=$(tr '\n' ' ' < "$DEVKITPRO/ounce-devkit-version.txt" 2>/dev/null || echo unknown)"
	echo "cpu=$SWITCH2_CPU"
	echo "mesa=$MESA_VERSION switch2 archive_sha256=$MESA_SDK_SHA256"
	echo "uam2rt=$(sed -n 's/^set(uam2rt_VERSION "\(.*\)")/\1/p' "$MESA_PREFIX/lib/cmake/uam2rt/uam2rtConfig.cmake")"
	echo "sdl=$SDL_REVISION patches=$SDL_PATCH_SUM"
	echo "ffmpeg=$FFMPEG_REVISION"
	echo "compiler=$(aarch64-none-elf-gcc -dumpfullversion)"
	echo "nro_sha256=$NRO_SHA256"
} > "$DIST_DIR/PPSSPP-NX2-build.txt"

echo "Built $DIST_DIR/PPSSPP-NX2.nro"
echo "SHA-256 $NRO_SHA256"
