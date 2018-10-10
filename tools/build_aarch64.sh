#!/bin/bash
set -e

if [[ "$EXTRA_CFLAGS" == "" ]]; then
    EXTRA_CFLAGS=" -O3 -fno-semantic-interposition -falign-functions=32 -D_FORTIFY_SOURCE=2 -fPIE"
fi

mkdir -p $HOME/build-aarch64
pushd $HOME/build-aarch64
make distclean || true
$SRCDIR/configure \
 --cross-prefix=aarch64-linux-gnu- \
 --disable-libiscsi \
 --disable-libnfs \
 --disable-libssh2 \
 --disable-linux-aio \
 --disable-lzo \
 --disable-bzip2 \
 --disable-modules \
 --disable-netmap \
 --disable-qom-cast-debug \
 --disable-seccomp \
 --disable-snappy \
 --disable-tcmalloc \
 --disable-tools \
 --disable-tpm \
 --disable-capstone \
 --disable-xen \
 --disable-xen-pci-passthrough \
 --disable-wdt \
 --disable-audio \
 --disable-bluetooth \
 --disable-usb-redir \
 --disable-spice \
 --disable-vnc \
 --disable-whpx \
 --disable-hvf \
 --disable-gtk \
 --disable-vte \
 --disable-sdl \
 --disable-rdma \
 --disable-vxhs \
 --disable-vvfat \
 --disable-parallels \
 --disable-dmg \
 --disable-tcg \
 --enable-attr \
 --enable-cap-ng \
 --enable-fdt \
 --enable-kvm \
 --enable-rbd \
 --enable-vhost-crypto \
 --enable-vhost-net \
 --enable-vhost-scsi \
 --enable-vhost-user \
 --enable-vhost-vsock \
 --enable-virtfs \
 --target-list=aarch64-softmmu \
 --extra-cflags="$EXTRA_CFLAGS" \
 --extra-ldflags=" -pie -z noexecstack -z relro -z now" \
 --libdir=/usr/lib64/nemu \
 --libexecdir=/usr/libexec/nemu \
 --datadir=/usr/share/nemu
$MAKEPREFIX make -j `nproc`
popd
