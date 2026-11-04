#!/bin/sh
# Run from a laptop with internet. The Pi has none, so this downloads LVGL and the
# libdrm headers here, copies everything to the Pi and builds the display test there.
#
# Usage: ./deploy.sh [ssh-host]    (default host: baja)
set -eu
cd "$(dirname "$0")"

HOST=${1:-baja}
LVGL_VERSION=v9.6.0
# Must match the libdrm2 version installed on the Pi: dpkg-query -W libdrm2
LIBDRM_DEB=libdrm-dev_2.4.134-3~bpo13+1+rpt1_arm64.deb
LIBDRM_URL=https://archive.raspberrypi.com/debian/pool/main/libd/libdrm/$LIBDRM_DEB

if [ ! -f third_party/lvgl/lvgl.h ]; then
    git clone --depth 1 --branch "$LVGL_VERSION" https://github.com/lvgl/lvgl.git third_party/lvgl
fi

if [ ! -f third_party/libdrm/include/xf86drm.h ]; then
    tmp=$(mktemp -d)
    curl -fsSL -o "$tmp/libdrm-dev.deb" "$LIBDRM_URL"
    (cd "$tmp" && ar x libdrm-dev.deb && tar -xf data.tar.*)
    mkdir -p third_party/libdrm/include
    cp -R "$tmp/usr/include/xf86drm.h" "$tmp/usr/include/xf86drmMode.h" "$tmp/usr/include/libdrm" \
        third_party/libdrm/include/
    rm -rf "$tmp"
fi

# The Pi's clock can be wrong, so "tar -m" stamps files with the Pi's own time.
# Otherwise make would see the sources as newer than what it just built.
copy_to_pi() {
    COPYFILE_DISABLE=1 tar -cf - --no-xattrs "$@" |
        ssh "$HOST" 'mkdir -p driver-display && tar -xmf - -C driver-display'
}

# Copying LVGL again would rebuild all of it, which takes minutes on the Pi.
if ! ssh "$HOST" test -f driver-display/third_party/lvgl/lvgl.h; then
    copy_to_pi third_party/libdrm third_party/lvgl/src third_party/lvgl/include \
        third_party/lvgl/lvgl.h third_party/lvgl/lvgl_private.h third_party/lvgl/lv_version.h
fi
copy_to_pi Makefile src

ssh "$HOST" 'make -C driver-display -j4 display-poc'
echo "Built. To run it: ssh -t $HOST ./driver-display/build/display-poc"
