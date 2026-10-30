#!/bin/sh
# Build ezinject (pinned) and libmytvhook.so for the webOS toolchain into build/homehook/.
#   homehook/build.sh <toolchain dir> <out dir>   (toolchain: the dir with bin/arm-webos-linux-gnueabi-gcc)
set -eu
TC=$1
B=$2
EZ_REPO=https://github.com/smx-smx/ezinject
EZ_REV=d9338952cd01597c5fdd7d6e98991474df5dc89c
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$B"
B=$(cd "$B" && pwd)
TCFILE=$TC/share/buildroot/toolchainfile.cmake

if [ ! -d "$B/ezinject-src/.git" ]; then
  git clone "$EZ_REPO" "$B/ezinject-src"
fi
git -C "$B/ezinject-src" checkout -q "$EZ_REV"
# Pristine pinned tree, then the myTV security patch (audit findings 1, 2, 4a). git apply fails
# loudly if the source moved from under the patch — the signal to re-audit before bumping EZ_REV.
git -C "$B/ezinject-src" reset --hard -q "$EZ_REV"
git -C "$B/ezinject-src" clean -fdq
git -C "$B/ezinject-src" apply "$HERE/ezinject-d9338952.patch"

cmake -S "$B/ezinject-src" -B "$B/ezinject-build" -DCMAKE_TOOLCHAIN_FILE="$TCFILE" \
  -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX="$B/ezinject"
# full build: `cmake --install` installs every target, and the CRT objects come with it
cmake --build "$B/ezinject-build"
cmake --install "$B/ezinject-build"

cmake -S "$HERE" -B "$B/hook-build" -DCMAKE_TOOLCHAIN_FILE="$TCFILE" -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCMAKE_PREFIX_PATH="$B/ezinject" -DCMAKE_FIND_ROOT_PATH="$B/ezinject"
cmake --build "$B/hook-build"

cp "$(find "$B/ezinject" -type f -name ezinject | head -n 1)" "$B/ezinject.bin"
cp "$B/hook-build/libmytvhook.so" "$B/libmytvhook.so"
ls -la "$B/ezinject.bin" "$B/libmytvhook.so"
