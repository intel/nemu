#!/bin/bash
set -x

GO_VERSION="1.10.3"
CLEAR_VERSION=24740
CLEAR_IMAGE=clear-$CLEAR_VERSION-cloud.img
UBUNTU_IMAGE=xenial-server-cloudimg-amd64-uefi1.img
WORKLOADS_DIR="$HOME/workloads"
OVMF="OVMF.fd"

go_install() {
    export PATH=/usr/local/go/bin:$PATH
    go version | grep $GO_VERSION
    if [ $? -ne 0 ]; then
	pushd /tmp
	wget -nv https://dl.google.com/go/go1.10.3.linux-amd64.tar.gz || exit $?
	sudo tar -C /usr/local -xzf go1.10.3.linux-amd64.tar.gz || exit $?
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
    wget -nv https://download.clearlinux.org/releases/$CLEAR_VERSION/clear/clear-$CLEAR_VERSION-cloud.img.xz || exit $?
    unxz clear-$CLEAR_VERSION-cloud.img.xz || exit $?
fi

if [ ! -f "$WORKLOADS_DIR"/"$UBUNTU_IMAGE" ]; then
   wget -nv https://cloud-images.ubuntu.com/xenial/current/xenial-server-cloudimg-amd64-uefi1.img || exit $?
   sudo apt-get install -y libguestfs-tools
   sudo mkdir -p /tmp/mnt
fi

rm -rf $OVMF
OVMF_URL=$(curl --silent https://api.github.com/repos/rbradford/edk2/releases/latest | grep -o https://.*OVMF.fd)
wget -nv $OVMF_URL || exit $?
popd

sudo adduser $USER kvm
pushd $SRCDIR/tools/CI/nats
newgrp kvm << EOF
go test -v -timeout 20m -parallel \$((`nproc`/2)) $@ || exit \$?
EOF
RES=$?
popd
exit $RES
