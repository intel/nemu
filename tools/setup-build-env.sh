#!/bin/sh
set -e
set -x

sudo sh -c 'echo "deb [arch=amd64] http://archive.ubuntu.com/ubuntu bionic main universe multiverse" > /etc/apt/sources.list'
sudo sh -c 'echo "deb [arch=amd64] http://archive.ubuntu.com/ubuntu bionic-updates main universe multiverse" >> /etc/apt/sources.list'
sudo sh -c 'echo "deb [arch=amd64] http://security.ubuntu.com/ubuntu bionic-security main universe multiverse" >> /etc/apt/sources.list'

sudo sh -c 'echo "deb-src http://archive.ubuntu.com/ubuntu bionic main universe multiverse" >> /etc/apt/sources.list'

sudo sh -c 'echo "APT::Install-Recommends \"0\";" >  /etc/apt/apt.conf.d/10local'
sudo sh -c 'echo "APT::Install-Suggests \"0\";" >>  /etc/apt/apt.conf.d/10local'

sudo sh -c 'echo "exit 101" > /usr/sbin/policy-rc.d'
sudo chmod a+x /usr/sbin/policy-rc.d

sudo apt-get update
sudo apt-get install -y build-essential flex bison git
sudo apt-get install -y mtools dosfstools
sudo apt-get build-dep -y qemu

