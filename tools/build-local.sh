#!/bin/bash
set -e

export PATH=$PATH:$(go env GOPATH)/bin
go get github.com/intel/ccloudvm/...
ccloudvm setup
tools/ccloudvm-build-aarch64.sh
tools/ccloudvm-build-x86_64.sh
