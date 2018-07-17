#!/bin/sh
export SRCDIR
MAKEPREFIX="strace -e open,openat -ff" $1 2>&1 | grep \\\.[ch]\" | grep -v "= -1" | sed -e "s/^.*\"\(.*\)\".*$/\1/" | grep ^$SRCDIR | sed -e s#^$SRCDIR/## | sort | uniq
