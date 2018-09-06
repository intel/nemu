#!/bin/bash
set -x

GO_VERSION="1.10.3"
CLEAR_VERSION=24740
CLEAR_IMAGE=clear-$CLEAR_VERSION-cloud.img.xz
WORKLOADS_DIR="$HOME/workloads"
OVMF="OVMF.fd"

go_install() {
    export PATH=/usr/local/go/bin:$PATH
    go version | grep $GO_VERSION
    if [ $? -ne 0 ]; then
	pushd /tmp
	wget -nv https://dl.google.com/go/go1.10.3.linux-amd64.tar.gz
	sudo tar -C /usr/local -xzf go1.10.3.linux-amd64.tar.gz
	popd
    fi

    export GOROOT=/usr/local/go
    export GOPATH=~/go

    go version
}

sudo apt-get install -y mtools dosfstools

go_install

go get -u github.com/intel/govmm/qemu
go get -u golang.org/x/crypto/ssh

mkdir -p $WORKLOADS_DIR
pushd $WORKLOADS_DIR

if [ ! -f "$WORKLOADS_DIR"/"$CLEAR_IMAGE" ]; then
    wget -nv https://download.clearlinux.org/releases/$CLEAR_VERSION/clear/clear-$CLEAR_VERSION-cloud.img.xz
    unxz clear-$CLEAR_VERSION-cloud.img.xz
fi

rm -rf $OVMF
OVMF_URL=$(curl --silent https://api.github.com/repos/rbradford/edk2/releases/latest | grep -o https://.*OVMF.fd)
wget $OVMF_URL
popd

sudo chmod a+rw /dev/kvm

pushd $SRCDIR/tools/CI/nats
go test -v -timeout 20m -parallel $((`nproc`/2))
popd
