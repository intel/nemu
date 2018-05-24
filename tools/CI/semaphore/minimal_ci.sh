#!/bin/bash
set -e

ARCH=x86_64
SRCDIR=~/nemu

sudo apt-get install -y mtools dosfstools

sudo -E $SRCDIR/tools/CI/minimal_ci.sh -hypervisor $HOME/build-$ARCH/$ARCH-softmmu/qemu-system-$ARCH -vsock false
