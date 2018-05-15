#!/bin/bash
set -e

mkdir -p $HOME/build-aarch64
pushd $HOME/build-aarch64
make distclean || true
$SRCDIR/configure \
 --cross-prefix=aarch64-linux-gnu- \
 --disable-guest-agent \
 --disable-guest-agent-msi \
 --disable-hax \
 --disable-libiscsi \
 --disable-libnfs \
 --disable-libssh2 \
 --disable-libusb \
 --disable-libxml2 \
 --disable-linux-aio \
 --disable-lzo \
 --disable-modules \
 --disable-netmap \
 --disable-opengl \
 --disable-qom-cast-debug \
 --disable-sdl \
 --disable-seccomp \
 --disable-smartcard \
 --disable-snappy \
 --disable-spice \
 --disable-tcg \
 --disable-tcmalloc \
 --disable-tools \
 --disable-tpm \
 --disable-usb-redir \
 --disable-vde \
 --disable-virtfs \
 --disable-vnc \
 --disable-vnc-jpeg \
 --disable-vnc-png \
 --disable-vnc-sasl \
 --disable-vxhs \
 --disable-xen \
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
 --target-list=aarch64-softmmu \
 --extra-cflags=" -O3 -fno-semantic-interposition -falign-functions=32 -D_FORTIFY_SOURCE=2 -fPIE" \
 --extra-ldflags=" -pie -z noexecstack -z relro -z now" \
 --libdir=/usr/lib64/nemu \
 --libexecdir=/usr/libexec/nemu \
 --datadir=/usr/share/nemu
$MAKEPREFIX make -j4
popd
