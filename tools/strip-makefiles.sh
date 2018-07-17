#!/bin/bash
set -e
set -x

ccloudvm start nemu-aarch64 || true
ccloudvm start nemu-x86-64 || true
ccloudvm run nemu-aarch64 cat $HOME/build-aarch64/aarch64-softmmu/*.mak $HOME/build-aarch64/config-host.mak > /tmp/aarch64-config
ccloudvm run nemu-x86-64 cat $HOME/build-x86_64/x86_64-softmmu/*.mak $HOME/build-x86_64/config-host.mak > /tmp/x86-64-config

cat /tmp/aarch64-config /tmp/x86-64-config | grep ^CONFIG | grep -v =\" | sort | uniq > /tmp/merged-config
find . | grep Makefile.objs | xargs -n 1 tools/makefile-cleaner.py /tmp/merged-config
