#!/usr/bin/env bash
#
# Build a shader-cache-enabled switch-mesa from the devkitPro Mesa fork with our
# Horizon disk-cache patch, and install it over the stock switch-mesa portlib.
#
# devkitPro ships switch-mesa with the on-disk shader cache disabled on Horizon,
# and even enabled it wouldn't work (no dl_iterate_phdr/mmap/flock/rename-on-FAT).
# Our patch (patches/mesa-switch-shadercache.patch) enables the cache and adapts
# disk_cache.c to Horizon so compiled shaders persist across launches.
#
# Meant to run inside the devkitpro/devkita64 CI container (Ubuntu, as root),
# but works on any Linux devkitPro install. Run BEFORE `make`.
set -euo pipefail

# Exact commit the stock switch-mesa (20.1.0) package is built from. Keep in sync
# with the installed package so the rebuilt libs stay ABI-compatible.
MESA_COMMIT=05fc4b1d449b8fe12cf1a47b46c4a1c1f4e4e3a6

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH="${SCRIPT_DIR}/../patches/mesa-switch-shadercache.patch"
: "${DEVKITPRO:=/opt/devkitpro}"

echo "==> Installing Mesa build dependencies"
apt-get update
apt-get install -y --no-install-recommends \
  git meson ninja-build bison flex python3-mako python3-setuptools
# dkp-meson-scripts provides /opt/devkitpro/meson-cross.sh
dkp-pacman -S --noconfirm --needed \
  dkp-meson-scripts dkp-toolchain-vars switch-pkg-config

echo "==> Fetching Mesa ${MESA_COMMIT} (shallow)"
rm -rf mesa-build
mkdir mesa-build
cd mesa-build
git init -q
git remote add origin https://github.com/devkitPro/mesa.git
git fetch -q --depth 1 origin "${MESA_COMMIT}"
git checkout -q FETCH_HEAD

echo "==> Applying shader-cache patch"
git apply --verbose "${PATCH}"

echo "==> Configuring + building Mesa (shader-cache enabled)"
"${DEVKITPRO}/meson-cross.sh" switch crossfile.txt build -Db_ndebug=true -Dshader-cache=true
ninja -C build

echo "==> Installing patched Mesa over the portlib"
ninja -C build install

echo "==> Done: patched switch-mesa installed."
