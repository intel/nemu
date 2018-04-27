#!/bin/bash
ccloudvm create --disk 10 --cpus 2 --mem 2048 --mount nemu,none,$(realpath $PWD/..) --name nemu-x86-64 xenial
ccloudvm start nemu-x86-64
ccloudvm run nemu-x86-64 $PWD/tools/setup-build-env.sh
ccloudvm run nemu-x86-64 SRCDIR=$PWD $PWD/tools/build_x86_64.sh
