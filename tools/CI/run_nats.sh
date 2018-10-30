#!/bin/bash
set -x

# Set to a specific git revision and repo to test, if empty or unset then will
# download latest tagged binary
#OVMF_GIT_REV="2f16693ce32cbe0956ce2dcaa571a32966413855"
#OVMF_GIT_REPO="https://github.com/intel/ovmf-virt"

GO_VERSION="1.11.1"
CLEAR_VERSION=25950
CLEAR_IMAGE=clear-$CLEAR_VERSION-cloud.img
UBUNTU_IMAGE=xenial-server-cloudimg-amd64-uefi1.img
WORKLOADS_DIR="$HOME/workloads"
OVMF="OVMF.fd"

# Matches the kernel for the $CLEAR_IMAGE above
CLEAR_KERNEL="org.clearlinux.kvm.4.18.16-293"

go_install() {
    export PATH=/usr/local/go/bin:$PATH
    go version | grep $GO_VERSION
    if [ $? -ne 0 ]; then
	pushd /tmp
	wget -nv "https://dl.google.com/go/go$GO_VERSION.linux-amd64.tar.gz" || exit $?
	sudo tar -C /usr/local -xzf "go$GO_VERSION.linux-amd64.tar.gz" || exit $?
	popd
    fi

    export GOROOT=/usr/local/go
    export GOPATH=~/go

    go version
}

sudo apt-get install -y mtools dosfstools

go_install

go get -u github.com/intel/govmm/qemu || exit $?
go get -u golang.org/x/crypto/ssh || exit $?

mkdir -p $WORKLOADS_DIR
pushd $WORKLOADS_DIR

if [ ! -f "$WORKLOADS_DIR"/"$CLEAR_IMAGE" ]; then
    wget -nv https://nemujenkinsstorage.blob.core.windows.net/images/clear-$CLEAR_VERSION-cloud.img.xz ||
    wget -nv https://download.clearlinux.org/releases/$CLEAR_VERSION/clear/clear-$CLEAR_VERSION-cloud.img.xz || exit $?
    unxz clear-$CLEAR_VERSION-cloud.img.xz || exit $?
fi


if [ ! -f "$WORKLOADS_DIR"/"$CLEAR_KERNEL" ]; then
    wget -nv https://nemujenkinsstorage.blob.core.windows.net/images/$CLEAR_KERNEL
fi

if [ ! -f "$WORKLOADS_DIR"/"$UBUNTU_IMAGE" ]; then
   wget -nv https://cloud-images.ubuntu.com/xenial/current/xenial-server-cloudimg-amd64-uefi1.img || exit $?
   sudo apt-get install -y libguestfs-tools
   sudo mkdir -p /tmp/mnt
   sudo guestmount -i -a  "$WORKLOADS_DIR"/"$UBUNTU_IMAGE" /tmp/mnt/
   sudo sed -i "s/console=tty1 console=ttyS0/console=tty1 console=ttyS0 console=hvc0/" /tmp/mnt/boot/grub/grub.cfg
   sudo umount /tmp/mnt
fi


rm -rf $OVMF
if [[ -z "$OVMF_GIT_REV" || -z "$OVMF_GIT_REPO" ]]; then
   OVMF_URL=$(curl --silent https://api.github.com/repos/intel/ovmf-virt/releases/latest | grep -o https://.*OVMF.fd)
   wget -nv $OVMF_URL || exit $?
else
   sudo apt-get install -y build-essential uuid-dev iasl git gcc-5 nasm
   git clone $OVMF_GIT_REPO || exit $?
   pushd ovmf-virt
   git checkout $OVMF_GIT_REV || exit $?
   make -C BaseTools || exit $?
   bash -c "export WORKSPACE=$PWD; . edksetup.sh; build"
   cp Build/OvmfX64/DEBUG_GCC5/FV/OVMF.fd "$WORKLOADS_DIR"/"$OVMF" || exit $?
   popd
   rm -rf ovmf-virt
fi

popd

sudo adduser $USER kvm
pushd $SRCDIR/tools/CI/nats
newgrp kvm << EOF
go test -v -timeout 20m -parallel \$((`nproc`/2)) $@ || exit \$?
EOF
RES=$?
popd
exit $RES
