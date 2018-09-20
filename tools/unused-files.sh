#!/bin/bash
set -e
ccloudvm run nemu-aarch64 SRCDIR=$PWD $PWD/tools/used_files.sh $PWD/tools/build_aarch64.sh > /tmp/used-files-aarch64
ccloudvm run nemu-x86-64 SRCDIR=$PWD $PWD/tools/used_files.sh $PWD/tools/build_x86_64.sh > /tmp/used-files-x86_64

cat /tmp/used-files-aarch64 /tmp/used-files-x86_64 | sort | uniq > /tmp/used-c-files
find . | grep \\.[ch]$ | sed "s#^./##" | sort | uniq > /tmp/all-c-files
comm -23 /tmp/all-c-files /tmp/used-c-files

