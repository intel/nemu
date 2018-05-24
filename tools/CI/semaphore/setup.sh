#!/bin/bash
set -e

# -fno-semantic-interposition is not supported by gcc 4.8 (from semaphore)
export EXTRA_CFLAGS=" -O3 -falign-functions=32 -D_FORTIFY_SOURCE=2 -fPIE"
export SRCDIR=~/nemu

sudo apt-get install -y libcap-ng-dev librbd-dev

$SRCDIR/tools/build_x86_64.sh

