#!/bin/sh
#
# Copy the standalone binaries to the running board.
#
# Usage:
#   ./standalone/deploy.sh [host] [remote_dir]
#
# Default remote_dir is /home/root (lost after reboot because the rootfs is
# initramfs). Use /run/media/mmcblk0p1 to keep them on the SD card.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
server="$root/build-standalone/canfd_server"
test_tool="$root/standalone/canfd_test"
host=${1:-192.168.8.24}
remote_dir=${2:-/home/root}

[ -x "$server" ] || {
    echo "missing $server; run ./standalone/build.sh first" >&2
    exit 1
}

ssh_opts="-O -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa"

scp $ssh_opts "$server" "root@$host:$remote_dir/"
[ -x "$test_tool" ] && scp $ssh_opts "$test_tool" "root@$host:$remote_dir/"

ssh -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    "root@$host" "chmod +x '$remote_dir/canfd_server' '$remote_dir/canfd_test'"

cat <<EOF

deployed to root@$host:$remote_dir

On the board:
  $remote_dir/canfd_test tx can0 0x123 01 02 03 04 05 06 07 08 --fd
  $remote_dir/canfd_server

EOF
