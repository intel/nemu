#!/bin/bash
set -e
if [ $# -le 2 ]; then
  REV="HEAD~1"
else
  REV=$1
fi

go get -u github.com/boyter/scc
export SRCDIR=$PWD;
pushd /tmp >/dev/null
git clone --shared $SRCDIR 2>/dev/null
pushd `basename $SRCDIR` >/dev/null
echo "Before:" 
git checkout $REV 2>/dev/null
scc --pathblacklist ".git,tests/" . | sed -n '1,5p'
echo
echo "After:" 
git checkout HEAD@{1} 2>/dev/null
scc --pathblacklist ".git,tests/" . | sed -n '1,5p'
popd > /dev/null
rm -rf `basename $SRCDIR`
popd > /dev/null
