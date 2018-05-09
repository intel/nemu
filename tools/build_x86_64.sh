#!/bin/bash
set -e

mkdir -p $HOME/build-x86_64
pushd $HOME/build-x86_64
make distclean || true
$SRCDIR/configure \
 --disable-debug-tcg \
 --disable-docs \
 --disable-fdt \
 --disable-gcrypt \
 --disable-glusterfs \
 --disable-gnutls \
 --disable-gtk \
 --disable-guest-agent \
 --disable-guest-agent-msi \
 --disable-hax \
 --disable-libiscsi \
 --disable-libnfs \
 --disable-libssh2 \
 --disable-libusb \
 --disable-libxml2 \
 --disable-linux-aio \
 --disable-linux-user \
 --disable-lzo \
 --disable-modules \
 --disable-netmap \
 --disable-nettle \
 --disable-opengl \
 --disable-qom-cast-debug \
 --disable-rdma \
 --disable-sdl \
 --disable-seccomp \
 --disable-smartcard \
 --disable-snappy \
 --disable-spice \
 --disable-tcg \
 --disable-tcg-interpreter \
 --disable-tcmalloc \
 --disable-tools \
 --disable-tpm \
 --disable-usb-redir \
 --disable-user \
 --disable-vde \
 --disable-virtfs \
 --disable-vnc \
 --disable-vnc-jpeg \
 --disable-vnc-png \
 --disable-vnc-sasl \
 --disable-vte \
 --disable-vxhs \
 --disable-xen \
 --enable-attr \
 --enable-cap-ng \
 --enable-kvm \
 --enable-rbd \
 --enable-vhost-crypto \
 --enable-vhost-net \
 --enable-vhost-scsi \
 --enable-vhost-user \
 --enable-vhost-vsock \
 --target-list=x86_64-softmmu \
 --extra-cflags=" -O3 -fno-semantic-interposition -falign-functions=32 -D_FORTIFY_SOURCE=2 -fPIE" \
 --extra-ldflags=" -pie -z noexecstack -z relro -z now" \
 --libdir=/usr/lib64/nemu \
 --libexecdir=/usr/libexec/nemu \
 --datadir=/usr/share/nemu
make -j 4
popd
