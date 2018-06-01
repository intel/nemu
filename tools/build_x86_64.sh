#!/bin/bash
set -e

if [[ "$EXTRA_CFLAGS" == "" ]]; then
    EXTRA_CFLAGS=" -O3 -fno-semantic-interposition -falign-functions=32 -D_FORTIFY_SOURCE=2 -fPIE"
fi

if [[ "$BUILD_DIR" == "" ]]; then
    BUILD_DIR="$HOME/build-x86_64"
fi

mkdir -p $BUILD_DIR
pushd $BUILD_DIR
make distclean || true
$SRCDIR/configure \
 --disable-fdt \
 --disable-libiscsi \
 --disable-libnfs \
 --disable-libssh2 \
 --disable-linux-aio \
 --disable-lzo \
 --disable-modules \
 --disable-netmap \
 --disable-qom-cast-debug \
 --disable-seccomp \
 --disable-snappy \
 --disable-tcmalloc \
 --disable-tools \
 --disable-tpm \
 --disable-virtfs \
 --enable-attr \
 --enable-cap-ng \
 --enable-kvm \
 --enable-rbd \
 --enable-vhost-crypto \
 --enable-vhost-net \
 --enable-vhost-scsi \
 --enable-vhost-user \
 --enable-vhost-vsock \
 --enable-virtfs \
 --target-list=x86_64-softmmu \
 --extra-cflags="$EXTRA_CFLAGS" \
 --extra-ldflags=" -pie -z noexecstack -z relro -z now" \
 --libdir=/usr/lib64/nemu \
 --libexecdir=/usr/libexec/nemu \
 --datadir=/usr/share/nemu
$MAKEPREFIX make -j `nproc`
popd
