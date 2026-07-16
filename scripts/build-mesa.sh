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
# Works both in the devkitpro/devkita64 CI container (Ubuntu, apt) and in the
# devkitPro MSYS2 shell on Windows (pacman). Run BEFORE `make`.
set -euo pipefail

# Exact commit the stock switch-mesa (20.1.0) package is built from. Keep in sync
# with the installed package so the rebuilt libs stay ABI-compatible.
MESA_COMMIT=05fc4b1d449b8fe12cf1a47b46c4a1c1f4e4e3a6

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH="${SCRIPT_DIR}/../patches/mesa-switch-shadercache.patch"
: "${DEVKITPRO:=/opt/devkitpro}"

# Stage the patched Mesa into a project-local dir instead of over the system
# portlib, so it never clobbers the toolchain's stock switch-mesa (which the
# user's other Switch ports link). The Makefile auto-detects and links it here.
MESA_INSTALL="$(cd "${SCRIPT_DIR}/.." && pwd)/mesa-install"

# The devkitPro pacman is `dkp-pacman` in the Linux container but plain `pacman`
# in the MSYS2 shell.
if command -v dkp-pacman >/dev/null 2>&1; then
  DKP_PACMAN=dkp-pacman
else
  DKP_PACMAN=pacman
fi

echo "==> Installing Mesa build dependencies"
if command -v apt-get >/dev/null 2>&1; then
  # Debian/Ubuntu (CI container)
  apt-get update
  apt-get install -y --no-install-recommends \
    git meson ninja-build bison flex python3-mako python3-setuptools
elif command -v pacman >/dev/null 2>&1; then
  # MSYS2 (devkitPro on Windows) -- different package names
  pacman -S --noconfirm --needed \
    git meson ninja bison flex python-mako python-setuptools
else
  echo "No apt-get or pacman found; install meson/ninja/bison/flex/python-mako manually." >&2
fi
# dkp-meson-scripts provides ${DEVKITPRO}/meson-cross.sh
"${DKP_PACMAN}" -S --noconfirm --needed \
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

echo "==> Staging patched Mesa into ${MESA_INSTALL} (system portlibs untouched)"
rm -rf "${MESA_INSTALL}"
DESTDIR="${MESA_INSTALL}" ninja -C build install

echo "==> Done: patched switch-mesa staged at ${MESA_INSTALL}."
echo "    Run 'make' to build the .nro (it links this staged Mesa automatically)."
