#!/bin/bash
set -e
set -x

sudo apt-get install -y mtools dosfstools

pushd /tmp
wget https://dl.google.com/go/go1.10.3.linux-amd64.tar.gz
sudo tar -C /usr/local -xzf go1.10.3.linux-amd64.tar.gz
popd

export PATH=/usr/local/go/bin:$PATH
export GOROOT=/usr/local/go

go version

export GOPATH=~/go

go get github.com/intel/govmm/qemu
go get golang.org/x/crypto/ssh

mkdir -p ~/workloads
pushd ~/workloads

CLEAR_VERSION=24740
wget https://download.clearlinux.org/releases/$CLEAR_VERSION/clear/clear-$CLEAR_VERSION-cloud.img.xz
unxz clear-$CLEAR_VERSION-cloud.img.xz

OVMF_URL=$(curl --silent https://api.github.com/repos/rbradford/edk2/releases/latest | grep -o https://.*OVMF.fd)
wget $OVMF_URL
popd

sudo chmod a+rw /dev/kvm

pushd tools/CI/nats
# Allow total test time to run for 20 minutes
go test -v -timeout 20m
popd
