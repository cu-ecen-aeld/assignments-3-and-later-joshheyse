#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]; then
  echo "Using default directory ${OUTDIR} for output"
else
  OUTDIR=$1
  echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
  #Clone only if the repository does not exist.
  echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
  git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
  cd linux-stable
  echo "Checking out version ${KERNEL_VERSION}"
  git checkout ${KERNEL_VERSION}

  make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper
  make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
  # echo "CONFIG_BLK_DEV_RAM=y\nCONFIG_BLK_DEV_RAM_COUNT=1\nCONFIG_BLK_DEV_RAM_SIZE=131072" >>${OUTDIR}/linux-stable/.config
  make -j4 ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all
  make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} modules
  make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} dtbs
fi

cd "$FINDER_APP_DIR"

echo "Adding the Image in outdir"
cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]; then
  echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
  sudo rm -rf ${OUTDIR}/rootfs
fi

mkdir -p ${OUTDIR}/rootfs/{bin,dev,etc,home,lib,lib64,proc,sbin,sys,tmp,usr/{bin,lib,sbin},var/log}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]; then
  git clone git://busybox.net/busybox.git
  cd busybox
  git checkout ${BUSYBOX_VERSION}
  make distclean
  make defconfig
  make -j4 ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
  make CONFIG_PREFIX=${OUTDIR}/rootfs ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} install
else
  cd busybox
fi

# TODO: Make and install busybox

echo "Library dependencies"
${CROSS_COMPILE}readelf -a busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a busybox | grep "Shared library"

cd "$FINDER_APP_DIR"

CROSS_GCC="$(which ${CROSS_COMPILE}gcc))"
CROSS_DIR="$(dirname $(dirname ${CROSS_GCC}))"
cp $CROSS_DIR/${CROSS_COMPILE::-1}/libc/lib/ld-linux-aarch64.so.1 ${OUTDIR}/rootfs/lib
cp $CROSS_DIR/${CROSS_COMPILE::-1}/libc/lib64/libm.so.6 ${OUTDIR}/rootfs/lib64
cp $CROSS_DIR/${CROSS_COMPILE::-1}/libc/lib64/libresolv.so.2 ${OUTDIR}/rootfs/lib64
cp $CROSS_DIR/${CROSS_COMPILE::-1}/libc/lib64/libc.so.6 ${OUTDIR}/rootfs/lib64

# TODO: Make device nodes
sudo mknod -m 666 ${OUTDIR}/rootfs/dev/null c 1 3
sudo mknod -m 666 ${OUTDIR}/rootfs/dev/console c 5 1
sudo mknod ${OUTDIR}/rootfs/dev/ram b 1 0

# TODO: Clean and build the writer utility

cd "$FINDER_APP_DIR"
make clean
CROSS_COMPILE="${CROSS_COMPILE}" make

# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs

cp writer ${OUTDIR}/rootfs/home
cp finder.sh ${OUTDIR}/rootfs/home
cp finder-test.sh ${OUTDIR}/rootfs/home
cp autorun-qemu.sh ${OUTDIR}/rootfs/home

cp -r conf ${OUTDIR}/rootfs/home/conf

rm -f ${OUTDIR}/initramfs.cpio
# rm -f ${OUTDIR}/initramfs.cpio.gz
cd "$OUTDIR/rootfs"
find . | cpio -H newc -ov --owner root:root >${OUTDIR}/initramfs.cpio
cd $OUTDIR
# gzip -f ${OUTDIR}/initramfs.cpio
