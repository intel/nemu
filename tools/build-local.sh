#!/bin/bash
set -e

export PATH=$PATH:$(go env GOPATH)/bin
go get github.com/intel/ccloudvm/...
ccloudvm setup
tools/ccloudvm-build-aarch64.sh

# Workaround for ccloudvm stop not waiting until VM completely stopped
sleep 20

tools/ccloudvm-build-x86_64.sh
