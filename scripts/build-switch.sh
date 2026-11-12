#!/usr/bin/env bash

set -euo pipefail

export PATH="/usr/bin:/bin:${PATH:-}"

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)"
DEPS_DIR="$ROOT_DIR/build-switch-deps"
BUILD_DIR="$ROOT_DIR/build-switch-release"
DIST_DIR="$ROOT_DIR/dist"
SDL_SOURCE_DIR="$DEPS_DIR/sdl-src"
SDL_BUILD_DIR="$DEPS_DIR/sdl-build"
SDL_PREFIX="$DEPS_DIR/sdl-prefix"
FFMPEG_SOURCE_DIR="$DEPS_DIR/ffmpeg-src"
FFMPEG_BUILD_DIR="$DEPS_DIR/ffmpeg-build"
FFMPEG_PREFIX="$DEPS_DIR/ffmpeg-prefix"

SDL_REPOSITORY="https://github.com/devkitPro/SDL.git"
SDL_REVISION="c329016c0e338ab4397ad930f83330c9fe058348"
SDL_RECIPE_VERSION="3"
FFMPEG_REPOSITORY="https://github.com/hrydgard/ppsspp-ffmpeg.git"
FFMPEG_REVISION="1e3b4965632f60b1d85360261d1b9dd45444bc71"
FFMPEG_RECIPE_VERSION="9"
MESA_VERSION="26.2.7"
MESA_SDK_SHA256="6c919eb1bbb7df5729940b5b4ecaa5e0daaa05bd06ebde9f5b676ce6c3eff04b"

# Every recursive removal is restricted to generated directories under this checkout.
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
export PATH="$DEVKITPRO/portlibs/switch/bin:$DEVKITA64/bin:$PATH"
export LC_ALL=C
export TZ=UTC
export ZERO_AR_DATE=1

MESA_SDK_DIR="$DEPS_DIR/mesa-$MESA_VERSION-sdk"
MESA_PREFIX="${SWITCH_MESA_PREFIX:-$MESA_SDK_DIR/opt/devkitpro/portlibs/switch}"
if [[ -n "${SWITCH_MESA_SDK:-}" ]]; then
	if [[ ! -f "$MESA_SDK_DIR/.ppsspp-sdk-sha256" ]] || [[ "$(<"$MESA_SDK_DIR/.ppsspp-sdk-sha256")" != "$MESA_SDK_SHA256" ]]; then
		"${PYTHON:-python3}" "$ROOT_DIR/scripts/unpack-switch-mesa.py" "$SWITCH_MESA_SDK" "$MESA_SDK_DIR"
	fi
fi
# MSYS2 can have several Python installations; select one with the host Mako module.
UAM_HOST_PYTHON=""
for candidate in "${UAM_PYTHON:-}" "${PYTHON:-}" python3 python /clangarm64/bin/python.exe /mingw64/bin/python.exe /ucrt64/bin/python.exe; do
	if [[ -n "$candidate" ]] && command -v "$candidate" >/dev/null && "$candidate" -c 'import mako.template' >/dev/null 2>&1; then
		UAM_HOST_PYTHON="$(command -v "$candidate")"
		break
	fi
done
[[ -n "$UAM_HOST_PYTHON" ]] || { echo "Deko3D requires host Python with Mako. Install python-mako or set UAM_PYTHON." >&2; exit 1; }

JOBS="${JOBS:-16}"
SWITCH_RELEASE_VERSION="${PPSSPP_SWITCH_RELEASE_VERSION:-1.1.1}"

case "$JOBS" in
	''|*[!0-9]*) echo "JOBS must be a positive integer" >&2; exit 2 ;;
	0) echo "JOBS must be greater than zero" >&2; exit 2 ;;
esac

case "$SWITCH_RELEASE_VERSION" in
	''|*[!0-9A-Za-z.+-]*) echo "PPSSPP_SWITCH_RELEASE_VERSION contains invalid characters" >&2; exit 2 ;;
esac

for tool in cmake git make ninja sha256sum tar bison flex realpath aarch64-none-elf-gcc aarch64-none-elf-pkg-config; do
	command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done

[[ -f "$DEVKITPRO/cmake/Switch.cmake" ]] || { echo "Missing devkitPro Switch toolchain" >&2; exit 1; }
[[ -f "$MESA_PREFIX/lib/cmake/Vulkan/VulkanConfig.cmake" ]] || { echo "Set SWITCH_MESA_SDK to mesa-26.2.7-switch-unified-horizon-sdk.zip, or SWITCH_MESA_PREFIX to its extracted portlibs/switch directory" >&2; exit 1; }
grep -Fq "Vulkan_VERSION \"$MESA_VERSION\"" "$MESA_PREFIX/lib/cmake/Vulkan/VulkanConfig.cmake" || { echo "Mesa SDK $MESA_VERSION is required" >&2; exit 1; }

mkdir -p "$DEPS_DIR" "$DIST_DIR"

SDL_PATCH="$ROOT_DIR/SDL/switch-vulkan-window.patch"
SDL_PATCH_SUM="$(sha256sum "$SDL_PATCH" | cut -d' ' -f1)"
SDL_STAMP="$SDL_RECIPE_VERSION $SDL_REVISION $SDL_PATCH_SUM $MESA_SDK_SHA256"
if [[ ! -f "$SDL_PREFIX/.ppsspp-switch-sdl" ]] || [[ "$(<"$SDL_PREFIX/.ppsspp-switch-sdl")" != "$SDL_STAMP" ]]; then
	remove_build_dirs "$SDL_SOURCE_DIR" "$SDL_BUILD_DIR" "$SDL_PREFIX"
	git clone --no-checkout "$SDL_REPOSITORY" "$SDL_SOURCE_DIR"
	git -C "$SDL_SOURCE_DIR" checkout --detach "$SDL_REVISION"
	git -C "$SDL_SOURCE_DIR" apply "$SDL_PATCH"
	cmake -S "$SDL_SOURCE_DIR" -B "$SDL_BUILD_DIR" -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
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
		-DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -g0 -ffile-prefix-map=$ROOT_DIR=. -fmacro-prefix-map=$ROOT_DIR=. -DSDL_VIDEO_DRIVER_SWITCH=1 -DSDL_AUDIO_DRIVER_SWITCH=1 -DSDL_JOYSTICK_SWITCH=1"
	cmake --build "$SDL_BUILD_DIR" --target install --parallel "$JOBS"
	printf '%s\n' "$SDL_STAMP" > "$SDL_PREFIX/.ppsspp-switch-sdl"
fi

FFMPEG_STAMP="$FFMPEG_RECIPE_VERSION $FFMPEG_REVISION"
if [[ ! -f "$FFMPEG_PREFIX/.ppsspp-switch-ffmpeg" ]] || [[ "$(<"$FFMPEG_PREFIX/.ppsspp-switch-ffmpeg")" != "$FFMPEG_STAMP" ]]; then
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
			--extra-cflags="-D__SWITCH__ -D_GNU_SOURCE -O3 -DNDEBUG -g0 -flto=auto -fno-fat-lto-objects -ffile-prefix-map=$ROOT_DIR=. -fmacro-prefix-map=$ROOT_DIR=. -I$MESA_PREFIX/include -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE -pie -ffunction-sections -fdata-sections -ftls-model=local-exec" \
			--extra-cxxflags="-D__SWITCH__ -D_GNU_SOURCE -O3 -DNDEBUG -g0 -flto=auto -fno-fat-lto-objects -ffile-prefix-map=$ROOT_DIR=. -fmacro-prefix-map=$ROOT_DIR=. -I$MESA_PREFIX/include -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE -pie -ffunction-sections -fdata-sections -ftls-model=local-exec" \
			--extra-ldflags="-g0 -flto=auto -fPIE -pie -Wl,--build-id=none -L$MESA_PREFIX/lib -L$DEVKITPRO/libnx/lib" \
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
	printf '%s\n' "$FFMPEG_STAMP" > "$FFMPEG_PREFIX/.ppsspp-switch-ffmpeg"
fi

if git -C "$ROOT_DIR" rev-parse --verify HEAD >/dev/null 2>&1; then
	export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT_DIR" show -s --format=%ct HEAD)}"
fi

make -C "$ROOT_DIR/Switch/forwarder" clean
make -C "$ROOT_DIR/Switch/forwarder" -j"$JOBS" VERSION="$SWITCH_RELEASE_VERSION"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
	-U'OPENGL_*' -U'_OPENGL_*' -U'Vulkan_*' -U'_VULKAN_SWITCH_*' \
	-DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_PREFIX_PATH="$MESA_PREFIX;$SDL_PREFIX" \
	-DOpenGL_DIR="$MESA_PREFIX/lib/cmake/OpenGL" \
	-DVulkan_DIR="$MESA_PREFIX/lib/cmake/Vulkan" \
	-DSDL3_DIR="$SDL_PREFIX/lib/cmake/SDL3" \
	-DFFMPEG_DIR="$FFMPEG_PREFIX" \
	-DPPSSPP_SWITCH_RELEASE_VERSION="$SWITCH_RELEASE_VERSION" \
	-DUSE_LIBNX=ON \
	-DUAM_PYTHON="$UAM_HOST_PYTHON" \
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
cmake -E copy_if_different "$BUILD_DIR/PPSSPP.nro" "$DIST_DIR/PPSSPP.nro"

SOURCE_REVISION="$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
SOURCE_STATE="clean"
# MSYS and Windows Git can have different checkout line-ending settings.
if ! git -c core.autocrlf=input -C "$ROOT_DIR" diff --quiet --ignore-submodules HEAD -- 2>/dev/null || [[ -n "$(git -C "$ROOT_DIR" ls-files --others --exclude-standard)" ]]; then
	SOURCE_STATE="dirty"
fi
PPSSPP_SHA256="$(sha256sum "$DIST_DIR/PPSSPP.nro" | cut -d' ' -f1)"
{
	echo "PPSSPP Switch $SWITCH_RELEASE_VERSION"
	echo "frontend=sdl"
	echo "source=$SOURCE_REVISION"
	echo "source_state=$SOURCE_STATE"
	echo "mesa=$MESA_VERSION archive_sha256=$MESA_SDK_SHA256"
	echo "sdl=$SDL_REVISION patch=$SDL_PATCH_SUM"
	echo "ffmpeg=$FFMPEG_REVISION"
	echo "compiler=$(aarch64-none-elf-gcc -dumpfullversion)"
	echo "ppsspp_nro_sha256=$PPSSPP_SHA256"
} > "$DIST_DIR/PPSSPP-build.txt"

echo "Built $DIST_DIR/PPSSPP.nro"
echo "SHA-256 $PPSSPP_SHA256"

# Remove artifacts produced by the former dual-frontend build.
cmake -E rm -f "$DIST_DIR/PPSSPP-SDL.nro" "$DIST_DIR/PPSSPP-SDL-build.txt" \
	"$DIST_DIR/PPSSPP-Original.nro" "$DIST_DIR/PPSSPP-Original-build.txt"
