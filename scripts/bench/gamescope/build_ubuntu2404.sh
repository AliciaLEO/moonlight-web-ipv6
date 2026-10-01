#!/bin/bash
# Plan « Idées Punktfunk », chapter G (G0): gamescope 3.16.31 built on Ubuntu 24.04, which ships none,
# into ~/.local/opt/gamescope — a bench machine's way to the Steam Big Picture card (bench §8s.14).
# Nothing of the system's is replaced: what 24.04 has too old (libwayland 1.22, xkbcommon 1.6, pixman
# 0.42, wayland-protocols 1.45) is built from wlroots' own wraps and linked in statically.
#
#   build_ubuntu2404.sh [tag]       (default 3.16.31)
#
# Needs sudo once, for the build packages. The gamescope found by the product is ~/.local/bin/gamescope.
set -eu
TAG=${1:-3.16.31}
SRC=$HOME/src/gamescope
PREFIX=$HOME/.local/opt/gamescope

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  git meson ninja-build cmake pkg-config glslang-tools hwdata bison \
  libdrm-dev libvulkan-dev libpipewire-0.3-dev libx11-dev libx11-xcb-dev libxcursor-dev libxext-dev \
  libxfixes-dev libxi-dev libxrender-dev libxxf86vm-dev libxcomposite-dev libxdamage-dev libxmu-dev \
  libxres-dev libxtst-dev libcap-dev libinput-dev libseat-dev libudev-dev libei-dev libeis-dev \
  libluajit-5.1-dev libdecor-0-dev libsdl2-dev \
  libxcb-composite0-dev libxcb-ewmh-dev libxcb-icccm4-dev libxcb-render0-dev libxcb-res0-dev \
  libxcb-xfixes0-dev libxcb-xinput-dev libxcb-dri3-dev libxcb-present-dev libxcb-shm0-dev \
  libxcb-util-dev libxkbcommon-x11-dev libexpat1-dev libffi-dev libxml2-dev

mkdir -p "$(dirname "$SRC")"
[ -d "$SRC/.git" ] || git clone https://github.com/ValveSoftware/gamescope.git "$SRC"
cd "$SRC"
git fetch --tags --quiet
git checkout --quiet "$TAG"
git submodule update --init --recursive --quiet

# wlroots' wraps, where meson looks for them (the top project's folder), each saying what it
# provides — so gamescope's own wayland, xkbcommon and pixman come from them too: one copy of each.
for w in subprojects/wlroots/subprojects/*.wrap; do
  [ -e "subprojects/$(basename "$w")" ] || cp "$w" subprojects/
done
provide() { grep -q '^\[provide\]' "subprojects/$1" || printf '\n[provide]\n%s\n' "$2" >> "subprojects/$1"; }
provide wayland.wrap 'dependency_names = wayland-server, wayland-client, wayland-egl, wayland-cursor, wayland-scanner
program_names = wayland-scanner'
provide libxkbcommon.wrap 'dependency_names = xkbcommon'
provide wayland-protocols.wrap 'dependency_names = wayland-protocols'
provide pixman.wrap 'dependency_names = pixman-1'
# xkbcommon's head wants meson 1.4, 24.04 has 1.3.2: its 1.8.1 release, the oldest wlroots takes —
# a checkout an earlier run left at its head included.
sed -i 's/^revision = HEAD$/revision = xkbcommon-1.8.1/' subprojects/libxkbcommon.wrap
if [ -d subprojects/libxkbcommon/.git ]; then
  git -C subprojects/libxkbcommon fetch --tags --quiet
  git -C subprojects/libxkbcommon checkout --quiet xkbcommon-1.8.1
fi
# wayland-scanner as a subproject is an internal dependency, read by name rather than as a pkg-config
# variable.
sed -i -E "s/get_variable\(\s*pkgconfig\s*:\s*'wayland_scanner'\s*\)/get_variable('wayland_scanner')/" \
  protocol/meson.build
# gamescope includes <pixman-1/pixman.h>, the system's layout; the subproject's headers sit flat.
mkdir -p pixman-shim/pixman-1
ln -sf ../../subprojects/pixman/pixman/pixman.h pixman-shim/pixman-1/pixman.h
ln -sf ../../build/subprojects/pixman/pixman/pixman-version.h pixman-shim/pixman-1/pixman-version.h

rm -rf build
meson setup build --prefix="$PREFIX" --buildtype=release \
  --default-library=static -Dcpp_args="-I$SRC/pixman-shim" \
  --force-fallback-for=libliftoff,vkroots,wayland-server,wayland-client,wayland-scanner,wayland-egl,wayland-cursor,xkbcommon,wayland-protocols \
  -Dwayland:documentation=false -Dwayland:tests=false -Dwayland:dtd_validation=false \
  -Dlibxkbcommon:xkb-config-root=/usr/share/X11/xkb -Dlibxkbcommon:x-locale-root=/usr/share/X11/locale \
  -Dlibxkbcommon:enable-tools=false -Dlibxkbcommon:enable-x11=false -Dlibxkbcommon:enable-docs=false \
  -Dlibxkbcommon:enable-xkbregistry=false -Dlibxkbcommon:enable-wayland=false \
  -Dpipewire=enabled -Dinput_emulation=enabled -Drt_cap=enabled -Ddrm_backend=enabled \
  -Dsdl2_backend=disabled -Davif_screenshots=disabled -Dbenchmark=disabled \
  -Denable_openvr_support=false -Denable_tests=false -Denable_zenity=false
ninja -C build
# gamescope's files only: a subproject would install into /usr/lib/udev.
meson install -C build --skip-subprojects --quiet
mkdir -p "$HOME/.local/bin"
ln -sf "$PREFIX/bin/gamescope" "$HOME/.local/bin/gamescope"
# Its Vulkan WSI layer is left out of the loader's path on purpose: built here, it carries a libwayland
# of its own beside the app's, and vkcube froze behind it (bench §8s.14).
"$PREFIX/bin/gamescope" --version 2>&1 | grep -o 'gamescope version [^ ]*'
