#!/bin/bash
set -e

# -fno-semantic-interposition is not supported by gcc 4.8 (from semaphore)
export EXTRA_CFLAGS=" -O3 -falign-functions=32 -D_FORTIFY_SOURCE=2 -fPIE"
export SRCDIR=$SEMAPHORE_PROJECT_DIR

sudo apt-get update
sudo apt-get build-dep -y qemu

# x86_64 build test
tools/build_x86_64.sh
