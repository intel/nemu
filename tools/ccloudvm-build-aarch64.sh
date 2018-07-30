#!/bin/bash
set -e

# need bionic for vhost-vsock
ccloudvm create --disk 10 --cpus 2 --mem 2048 --name nemu-aarch64 bionic || ccloudvm start nemu-aarch64
ccloudvm run nemu-aarch64 mkdir src
ccloudvm copy nemu-aarch64 --recurse $PWD src/nemu
ccloudvm run nemu-aarch64 src/nemu/tools/setup-cross-env.sh
ccloudvm run nemu-aarch64 SRCDIR='$HOME/src/nemu' src/nemu/tools/build_aarch64.sh
ccloudvm stop nemu-aarch64
