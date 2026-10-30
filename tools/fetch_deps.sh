#!/bin/sh
# Download the webOS cross toolchain (openlgtv/buildroot-nc4) and SDL2 for webOS (webosbrew/SDL-webOS)
# into <dir>. The toolchain is published for Linux x86_64 and aarch64 hosts only; on macOS build
# inside a Linux container or point TOOLCHAIN= at a toolchain you built yourself.
set -eu
DIR=${1:-deps}
TC_TAG=2026.08-webos
SDL_TAG=release-2.30.12-webos.5
SDL_TAR=SDL2-2.30.12-webos.tar.gz

case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) HOST=x86_64 ;;
  Linux-aarch64) HOST=aarch64 ;;
  *) echo "no prebuilt toolchain for $(uname -s)-$(uname -m); see README" >&2; exit 1 ;;
esac

mkdir -p "$DIR/sdl"
if [ ! -x "$DIR/arm-webos-linux-gnueabi_sdk-buildroot/bin/arm-webos-linux-gnueabi-gcc" ]; then
  curl -fL "https://github.com/openlgtv/buildroot-nc4/releases/download/$TC_TAG/arm-webos-linux-gnueabi_sdk-buildroot-$HOST.tar.gz" \
    | tar -xz -C "$DIR"
  "$DIR/arm-webos-linux-gnueabi_sdk-buildroot/relocate-sdk.sh"
fi
if [ ! -e "$DIR/sdl/lib/libSDL2-2.0.so.0" ]; then
  curl -fL "https://github.com/webosbrew/SDL-webOS/releases/download/$SDL_TAG/$SDL_TAR" | tar -xz -C "$DIR/sdl"
  # the archive may wrap everything in a prefix directory: flatten it to sdl/{include,lib}
  inc=$(find "$DIR/sdl" -type d -path '*/include/SDL2' | head -n 1)
  top=${inc%/include/SDL2}
  if [ "$top" != "$DIR/sdl" ]; then cp -R "$top/." "$DIR/sdl/"; fi
fi
echo "deps ready in $DIR"
