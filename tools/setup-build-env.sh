#!/bin/sh
set -e
set -x

export HOST_OS_RELEASE=`lsb_release -c -s`

sudo -E sh -c 'echo "deb [arch=amd64] http://archive.ubuntu.com/ubuntu $HOST_OS_RELEASE main universe multiverse" > /etc/apt/sources.list'
sudo -E sh -c 'echo "deb [arch=amd64] http://archive.ubuntu.com/ubuntu $HOST_OS_RELEASE-updates main universe multiverse" >> /etc/apt/sources.list'
sudo -E sh -c 'echo "deb [arch=amd64] http://security.ubuntu.com/ubuntu $HOST_OS_RELEASE-security main universe multiverse" >> /etc/apt/sources.list'

sudo -E sh -c 'echo "deb-src http://archive.ubuntu.com/ubuntu $HOST_OS_RELEASE main universe multiverse" >> /etc/apt/sources.list'

sudo sh -c 'echo "APT::Install-Recommends \"0\";" >  /etc/apt/apt.conf.d/10local'
sudo sh -c 'echo "APT::Install-Suggests \"0\";" >>  /etc/apt/apt.conf.d/10local'

sudo sh -c 'echo "exit 101" > /usr/sbin/policy-rc.d'
sudo chmod a+x /usr/sbin/policy-rc.d

sudo apt-get update
sudo apt-get install -y build-essential flex bison git
sudo apt-get install -y mtools dosfstools
sudo apt-get build-dep -y qemu

