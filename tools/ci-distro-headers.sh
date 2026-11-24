#!/bin/sh
# Build the modules against the newest packaged generic kernel headers of
# the Ubuntu (or Debian-family) release this runs on, for CI.
#
# Usage: tools/ci-distro-headers.sh   (as root, in a throwaway container)
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)

export DEBIAN_FRONTEND=noninteractive
apt-get -qq update
apt-get -qq install -y --no-install-recommends \
	build-essential dwarves kmod linux-headers-generic > /dev/null

# linux-headers-generic depends on exactly one versioned headers package.
KDIR=$(ls -d /usr/src/linux-headers-*-generic | sort -V | tail -n 1)
KVER=${KDIR#/usr/src/linux-headers-}
. /etc/os-release
echo "Building against $KVER headers on $PRETTY_NAME, $(gcc --version | head -n 1)"

make -C "$KDIR" M="$REPO" KCFLAGS=-Werror modules
modinfo -F vermagic "$REPO"/tbs6814.ko
