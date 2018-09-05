#!/bin/bash
set -e

# Our default Jenkins host OS is Ubuntu Xenial (16.04)
ccloudvm create --disk 30 --cpus `nproc` --mem 2048 --mount nemu,none,$(realpath $PWD) --name nemu-nats-x86-64 xenial || ccloudvm start nemu-nats-x86-64
ccloudvm run nemu-nats-x86-64 OS_VERSION="xenial" $PWD/tools/setup-build-env.sh
ccloudvm run nemu-nats-x86-64 SRCDIR=$PWD $PWD/tools/build_x86_64.sh
ccloudvm run nemu-nats-x86-64 SRCDIR=$PWD $PWD/tools/CI/run_nats.sh
ccloudvm stop nemu-nats-x86-64
