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
# git://busybox.net is unreliable, use the GitHub mirror with busybox.net https as fallback
BUSYBOX_REPO=https://github.com/mirror/busybox.git
BUSYBOX_MIRROR=https://git.busybox.net/busybox

# Use sudo only when not already root (the autotest container runs as root)
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO=sudo
fi

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}
OUTDIR=$(realpath ${OUTDIR})

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

    # Deep clean, configure for the QEMU "virt" arm64 target, then build
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} dtbs
fi

echo "Adding the Image in outdir"
cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}/

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    ${SUDO} rm  -rf ${OUTDIR}/rootfs
fi

mkdir -p ${OUTDIR}/rootfs
cd ${OUTDIR}/rootfs
mkdir -p bin dev etc home lib lib64 proc sbin sys tmp usr var
mkdir -p usr/bin usr/lib usr/sbin
mkdir -p var/log

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
    git clone ${BUSYBOX_REPO} --depth 1 --single-branch --branch ${BUSYBOX_VERSION} busybox \
        || git clone ${BUSYBOX_MIRROR} --depth 1 --single-branch --branch ${BUSYBOX_VERSION} busybox
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    make distclean
    make defconfig
    # The tc applet does not build against recent kernel headers (CBQ was removed)
    sed -i 's/^CONFIG_TC=y$/# CONFIG_TC is not set/' .config
else
    cd busybox
fi

make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
make CONFIG_PREFIX=${OUTDIR}/rootfs ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} install

cd ${OUTDIR}/rootfs
echo "Library dependencies"
${CROSS_COMPILE}readelf -a bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a bin/busybox | grep "Shared library"

# Copy the program interpreter and shared libraries from the toolchain sysroot,
# following symlinks so the rootfs gets the real files
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)
INTERPRETER=$(${CROSS_COMPILE}readelf -a bin/busybox | grep "program interpreter" | sed -e 's/.*: \(.*\)\]/\1/')
cp -L ${SYSROOT}${INTERPRETER} ${OUTDIR}/rootfs/lib/
for lib in $(${CROSS_COMPILE}readelf -a bin/busybox | grep "Shared library" | sed -e 's/.*\[\(.*\)\]/\1/')
do
    cp -L ${SYSROOT}/lib64/${lib} ${OUTDIR}/rootfs/lib64/
done

${SUDO} mknod -m 666 dev/null c 1 3
${SUDO} mknod -m 600 dev/console c 5 1

cd ${FINDER_APP_DIR}
make clean
make CROSS_COMPILE=${CROSS_COMPILE}

cp ${FINDER_APP_DIR}/writer ${OUTDIR}/rootfs/home/
cp ${FINDER_APP_DIR}/finder.sh ${OUTDIR}/rootfs/home/
cp ${FINDER_APP_DIR}/finder-test.sh ${OUTDIR}/rootfs/home/
cp ${FINDER_APP_DIR}/autorun-qemu.sh ${OUTDIR}/rootfs/home/
# finder-app/conf is a symlink to ../conf, copy the files it points to
mkdir -p ${OUTDIR}/rootfs/home/conf
cp -L ${FINDER_APP_DIR}/conf/username.txt ${FINDER_APP_DIR}/conf/assignment.txt ${OUTDIR}/rootfs/home/conf/

${SUDO} chown -R root:root ${OUTDIR}/rootfs

cd ${OUTDIR}/rootfs
find . | cpio -H newc -o --owner root:root > ${OUTDIR}/initramfs.cpio
gzip -f ${OUTDIR}/initramfs.cpio
