#!/bin/bash
set -e

# Install go 1.10.1
sudo add-apt-repository -y ppa:gophers/archive
sudo apt-get update
sudo apt-get install -y golang-1.10-go

# Setup golang
mkdir -p ~/go
export GOROOT=/usr/lib/go-1.10
export GOPATH=~/go
export PATH=$GOPATH/bin:$GOROOT/bin:$PATH

sudo apt-get update
sudo apt-get install qemu xorriso -y
sudo chmod ugo+rwx /dev/kvm

# Install ccloudvm
go version
go get github.com/intel/ccloudvm/...

# ARM64 build test
ccvm --systemd=false &
tools/ccloudvm-build-aarch64.sh
