#!/bin/sh
#
# Refresh the canfd-server recipe from the current workspace src/ tree and
# install it into the PetaLinux SDK.
#
# Usage:
#   ./petalinux/sync-to-petalinux.sh [PETA_SDK_CANFD_DIR]
#
# The optional argument is the CanFD directory that contains zmht_canfd.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
recipe="$root/petalinux/canfd-server"
sdk_canfd=${1:-/home/wang/ZMHT-CTHT-SDK/xilinx-linux-ctht-sdk/CanFD}
apps="$sdk_canfd/zmht_canfd/project-spec/meta-user/recipes-apps"

tar czf "$recipe/files/canfd-src.tar.gz" -C "$root" src
echo "updated: $recipe/files/canfd-src.tar.gz"

if [ ! -d "$apps" ]; then
    echo "PetaLinux recipes-apps not found: $apps" >&2
    exit 1
fi

if [ -d "$apps/canfd-server" ]; then
    cp -a "$apps/canfd-server" "$apps/canfd-server.bak.$(date +%Y%m%d-%H%M%S)"
fi
rm -rf "$apps/canfd-server"
cp -a "$recipe" "$apps/"
echo "installed: $apps/canfd-server"
echo
echo "Next (inside the PetaLinux container):"
echo "  source /opt/petalinux/2019.1/settings.sh"
echo "  cd /workspace/sdk/CanFD/zmht_canfd"
echo "  petalinux-build -c canfd-server"
echo "  petalinux-build"
