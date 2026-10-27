#!/bin/sh
#
# Cross-compile the CANFD userspace app and the direct SocketCAN test tool.
# The resulting binaries run on the original PetaLinux image; nothing is
# installed into image.ub.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tc=/home/wang/ZMHT-Tool/gcc-arm-linux-gnueabi/bin
build="$root/build-standalone"

cmake -S "$root" -B "$build"
cmake --build "$build" -j4

"$tc/arm-linux-gnueabihf-gcc" -O2 -static \
    -o "$root/standalone/canfd_test" "$root/standalone/canfd_test.c"

echo
echo "=== canfd_server ==="
file "$build/canfd_server"
"$tc/arm-linux-gnueabihf-readelf" -d "$build/canfd_server" | grep NEEDED || true

echo
echo "=== canfd_test ==="
file "$root/standalone/canfd_test"
