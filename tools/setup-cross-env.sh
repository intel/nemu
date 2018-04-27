#!/bin/sh
set -x
sudo dpkg --add-architecture arm64

sudo sh -c 'echo "deb [arch=amd64] http://archive.ubuntu.com/ubuntu xenial main universe multiverse" > /etc/apt/sources.list'
sudo sh -c 'echo "deb [arch=amd64] http://archive.ubuntu.com/ubuntu xenial-updates main universe multiverse" >> /etc/apt/sources.list'
sudo sh -c 'echo "deb [arch=amd64] http://security.ubuntu.com/ubuntu xenial-security main universe multiverse" >> /etc/apt/sources.list'

sudo sh -c 'echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports xenial main universe multiverse" >> /etc/apt/sources.list'
sudo sh -c 'echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports xenial-updates main universe multiverse" >> /etc/apt/sources.list'

sudo sh -c 'echo "deb-src http://archive.ubuntu.com/ubuntu xenial main universe multiverse" >> /etc/apt/sources.list'

sudo sh -c 'echo "APT::Install-Recommends \"0\";" >  /etc/apt/apt.conf.d/10local'
sudo sh -c 'echo "APT::Install-Suggests \"0\";" >>  /etc/apt/apt.conf.d/10local'

sudo sh -c 'echo "exit 101" > /usr/sbin/policy-rc.d'
sudo chmod a+x /usr/sbin/policy-rc.d

sudo apt-get update
sudo apt-get install -y g++-aarch64-linux-gnu build-essential flex bison git

sudo apt-get build-dep -y -aarm64 qemu

