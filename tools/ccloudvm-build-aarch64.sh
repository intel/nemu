#!/bin/bash
set -e

# need bionic for vhost-vsock
ccloudvm create --disk 10 --cpus 2 --mem 2048 --mount nemu,none,$(realpath $PWD) --name nemu-aarch64 bionic || ccloudvm start nemu-aarch64
ccloudvm run nemu-aarch64 $PWD/tools/setup-cross-env.sh
ccloudvm run nemu-aarch64 SRCDIR=$PWD $PWD/tools/build_aarch64.sh
ccloudvm stop nemu-aarch64
