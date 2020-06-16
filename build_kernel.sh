#!/bin/bash
export KERNELDIR=`readlink -f .`
export RAMFS_SOURCE=`readlink -f $KERNELDIR/ramdisk`
export PARTITION_SIZE=100663296

export OUT_DIR=../out

export OS="10.0.0"
export SPL="2020-03"

echo "kerneldir = $KERNELDIR"
echo "ramfs_source = $RAMFS_SOURCE"

RAMFS_TMP="/tmp/arter97-op7-ramdisk"

echo "ramfs_tmp = $RAMFS_TMP"
cd $KERNELDIR

stock=0
if [[ "${1}" == "stock" ]] ; then
	stock=1
	shift
fi

MAKE_FLAGS="O=$OUT_DIR ARCH=arm64 CROSS_COMPILE=../arm64-binutils/bin/aarch64-linux-androidkernel- CLANG_TRIPLE=aarch64-linux-gnu"

if [[ "${1}" == "skip" ]] ; then
	echo "Skipping Compilation"
else
	echo "Compiling kernel"
	make $MAKE_FLAGS vendor/sm8150-perf_defconfig
	make $MAKE_FLAGS "$@" || exit 1
fi

echo "Building new ramdisk"
#remove previous ramfs files
rm -rf '$RAMFS_TMP'*
rm -rf $RAMFS_TMP
rm -rf $RAMFS_TMP.cpio
#copy ramfs files to tmp directory
cp -axpP $RAMFS_SOURCE $RAMFS_TMP
cd $RAMFS_TMP

find . -name EMPTY_DIRECTORY -exec rm -rf {} \;

$KERNELDIR/ramdisk_fix_permissions.sh 2>/dev/null

cd $KERNELDIR
rm -rf $RAMFS_TMP/tmp/*

cd $RAMFS_TMP
find . | fakeroot cpio -H newc -o | gzip -9 > $RAMFS_TMP.cpio.gz
ls -lh $RAMFS_TMP.cpio.gz
cd $KERNELDIR

echo "Making new boot image"
find $OUT_DIR/arch/arm64/boot/dts -name '*.dtb' -exec cat {} + > $RAMFS_TMP.dtb
gcc -O2 -Iscripts/mkbootimg \
    scripts/mkbootimg/mkbootimg.c \
    scripts/mkbootimg/libmincrypt/sha.c \
    scripts/mkbootimg/libmincrypt/sha256.c -o scripts/mkbootimg/mkbootimg
scripts/mkbootimg/mkbootimg \
    --kernel $OUT_DIR/arch/arm64/boot/Image.gz \
    --ramdisk $RAMFS_TMP.cpio.gz \
    --cmdline 'androidboot.hardware=qcom androidboot.console=ttyMSM0 androidboot.memcg=1 lpm_levels.sleep_disabled=1 video=vfb:640x400,bpp=32,memsize=3072000 msm_rtb.filter=0x237 service_locator.enable=1 swiotlb=2048 firmware_class.path=/vendor/firmware_mnt/image loop.max_part=7 androidboot.usbcontroller=a600000.dwc3 buildvariant=user printk.devkmsg=on ramdisk_size=4812800' \
    --base           0x00000000 \
    --pagesize       4096 \
    --kernel_offset  0x00008000 \
    --ramdisk_offset 0x01000000 \
    --second_offset  0x00f00000 \
    --tags_offset    0x00000100 \
    --dtb            $RAMFS_TMP.dtb \
    --dtb_offset     0x01f00000 \
    --os_version     $OS \
    --os_patch_level $SPL \
    --header_version 2 \
    -o $KERNELDIR/boot.img

GENERATED_SIZE=$(stat -c %s boot.img)
if [[ $GENERATED_SIZE -gt $PARTITION_SIZE ]]; then
	echo "boot.img size larger than partition size!" 1>&2
	exit 1
fi

echo "done"
ls -al boot.img
echo ""
