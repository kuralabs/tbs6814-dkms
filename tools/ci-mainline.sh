#!/bin/sh
# Build the modules against a mainline kernel tree, for CI.
#
# Usage: tools/ci-mainline.sh KERNEL_SRC_DIR
#
# Configures KERNEL_SRC_DIR as a minimal x86_64 kernel (allnoconfig plus
# what these modules need), builds it so Module.symvers lists the real
# exports, then builds tbs6814.ko and mtv23x.ko against it with -Werror.
# modpost fails on any symbol the kernel no longer exports.
set -eu

KSRC=$(cd "$1" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
JOBS=$(nproc)

cd "$KSRC"
make -s allnoconfig
./scripts/config \
	-e 64BIT -e MODULES -e MODULE_UNLOAD \
	-e PCI -e PCI_MSI \
	-e I2C \
	-e MEDIA_SUPPORT -d MEDIA_SUPPORT_FILTER \
	-e MEDIA_DIGITAL_TV_SUPPORT -e MEDIA_PCI_SUPPORT \
	-m DVB_CORE \
	-d MEDIA_SUBDRV_AUTOSELECT -e MEDIA_HIDE_ANCILLARY_SUBDRV \
	-m DVB_MN88472
make -s olddefconfig

# DVB_MN88472 is only there to select REGMAP_I2C, which has no prompt.
for sym in MODULES PCI_MSI I2C DVB_CORE REGMAP_I2C; do
	grep -qE "^CONFIG_${sym}=(y|m)$" .config || {
		echo "error: CONFIG_${sym} is not enabled in the CI kernel config" >&2
		exit 1
	}
done

echo "Building kernel $(make -s kernelversion) ($JOBS jobs)..."
make -s -j"$JOBS"

echo "Building tbs6814 against it..."
make -C "$KSRC" M="$REPO" KCFLAGS=-Werror modules
ls -l "$REPO"/tbs6814.ko "$REPO"/mtv23x.ko
