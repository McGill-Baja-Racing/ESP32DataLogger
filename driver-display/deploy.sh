#!/bin/sh
# Run from a laptop with internet. The Pi has none, so this downloads LVGL and the
# libdrm headers here, copies everything to ~/driver-display on the Pi and builds there.
#
# Usage: ./deploy.sh [ssh-host]    (default host: baja)
#        ./deploy.sh --fetch-only  (only download, to build on this computer)
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

if [ "$HOST" = "--fetch-only" ]; then
    exit 0
fi

# Only files whose content changed are replaced on the Pi, and they get the Pi's
# time. make then rebuilds exactly what changed, even though the Pi's clock is wrong.
COPYFILE_DISABLE=1 tar -cf - --no-xattrs Makefile src systemd third_party/libdrm \
    third_party/lvgl/src third_party/lvgl/include third_party/lvgl/lvgl.h \
    third_party/lvgl/lvgl_private.h third_party/lvgl/lv_version.h |
    ssh "$HOST" 'set -e
        rm -rf .driver-display-upload
        mkdir .driver-display-upload
        tar -xmf - -C .driver-display-upload
        cd .driver-display-upload
        changed=0
        for f in $(find . -type f); do
            if ! cmp -s "$f" "../driver-display/$f"; then
                mkdir -p "../driver-display/$(dirname "$f")"
                cp "$f" "../driver-display/$f"
                changed=$((changed + 1))
            fi
        done
        cd ..
        rm -rf .driver-display-upload
        echo "$changed files updated on the Pi"'

ssh "$HOST" 'make -C driver-display -j4 --no-print-directory'

echo "Built. To run it: ssh -t $HOST ./driver-display/build/driver-display"
if ssh "$HOST" systemctl is-active --quiet driver-display; then
    echo "Autostart is on, so the old version is still running. Restart it with:"
    echo "  ssh -t $HOST sudo systemctl restart driver-display"
fi
