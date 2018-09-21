#!/bin/bash
set -e

sudo apt-get install -y xorriso

export GOROOT=/usr/local/go
export PATH=/usr/local/go/bin:$PATH
export GOPATH=$HOME/go
go get -u github.com/intel/ccloudvm/...
export PATH=$GOPATH/bin:$PATH

pushd $SRCDIR
sudo adduser $USER kvm
newgrp kvm << EOF
ccvm -systemd=false &
EOF

newgrp kvm << EOF
ccloudvm create --cpus 8 --disk 10 --mem 8192 --mount nemu,none,\$(realpath $PWD/..) --name nemu-x86-64 bionic || exit $?
ccloudvm run nemu-x86-64 \$PWD/tools/setup-build-env.sh || exit $?
EOF

newgrp kvm << EOF
ccloudvm create --cpus 8 --disk 10 --mem 8192 --mount nemu,none,\$(realpath $PWD/..)  --name nemu-aarch64 bionic || exit $?
ccloudvm run nemu-aarch64 \$PWD/tools/setup-cross-env.sh || exit $?
EOF

newgrp kvm << EOF
tools/unused-files.sh > /tmp/unused-files
EOF

xargs -n 1 git rm <  /tmp/unused-files 
git commit -a --author="NEMU CI <noreply@intel.com>" -m "Automatic code removal"
git push origin HEAD:experiment/virt-code-reduction

popd