#!/bin/bash
set -x

UBUNTU_IMAGE=xenial-server-cloudimg-arm64-uefi1.img
WORKLOADS_DIR="$HOME/workloads"

export GOROOOT=/usr/local/go
export PATH=/usr/local/go/bin:$PATH
export GOPATH=$HOME/go

go get -u github.com/intel/govmm/qemu || exit $?
go get -u golang.org/x/crypto/ssh || exit $?

mkdir -p $WORKLOADS_DIR

if [ ! -f "$WORKLOADS_DIR"/"$UBUNTU_IMAGE" ]; then
   pushd $WORKLOADS_DIR
   wget -4 -nv https://cloud-images.ubuntu.com/xenial/current/xenial-server-cloudimg-arm64-uefi1.img || exit $?
   sudo apt-get install -y libguestfs-tools
   sudo mkdir -p /tmp/mnt
   sudo guestmount -i -a  "$WORKLOADS_DIR"/"$UBUNTU_IMAGE" /tmp/mnt/
   sudo sed -i "s/quiet splash/console=ttyAMA0 console=hvc0/" /tmp/mnt/boot/grub/grub.cfg
   sudo umount /tmp/mnt
   popd
fi

pushd $SRCDIR/tools/CI/nats
go test -v -timeout 20m -parallel $((`nproc`/2)) || exit $?
popd
