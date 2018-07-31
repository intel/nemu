#!/bin/bash
# -*- mode: shell-script; indent-tabs-mode: nil; sh-basic-offset: 4; -*-
# ex: ts=8 sw=4 sts=4 et filetype=sh
#
#  start_qemu.sh
#
#  Copyright (c) 2016-2018 Intel Corporation

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rightS
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#

SCRIPT_DIR="`dirname "$0"`" 
CI_SCRIPT="$SCRIPT_DIR/start_qemu.sh"
CLOUD_INIT_DIR="$SCRIPT_DIR/cloud-init/"
BUILD_DIR=""
HYPERVISOR="$HOME/build-x86_64/x86_64-softmmu/qemu-system-x86_64"
WORKLOADS_DIR="$HOME/workloads"
EFI_FIRMWARE_URI=$(curl --silent https://api.github.com/repos/rbradford/edk2/releases/latest | grep -o https://.*OVMF.fd)
DOWNLOAD_ONLY="false"
RUN_UNSAFE="false"
PIIX_TEST="false"
VERBOSE="false"
CHECK="false"
VSOCK="true"

die(){
   echo "${1}"
   exit 1
}

# The array of test images is the input
run_tests() {
   local testarray=("$@")
   local testcount=${#testarray[@]}
   local index=0

   while [ "$index" -lt $testcount ]
   do
      image=${testarray[index]}
      ((index++))
      format=${testarray[index]}
      ((index++))
      firmware=${testarray[index]}
      ((index++))
      platform=${testarray[index]}
      ((index++))
      extra_args=${testarray[index]}
      ((index++))
      uri=${testarray[index]}
      ((index++))

       if [ ! -f "$WORKLOADS_DIR"/"$image" ]; then
          pushd "$WORKLOADS_DIR"
          wget --no-verbose "$uri"
          popd
       fi
   
       if [[ -f "$WORKLOADS_DIR"/"$image".xz ]]; then
          unxz "$WORKLOADS_DIR"/"$image".xz
       fi
   
       if [[ -f "$WORKLOADS_DIR"/"$image".bz2 ]]; then
          bunzip2 "$WORKLOADS_DIR"/"$image".bz2
       fi

       if [[ "$VERBOSE" == "true" ]]; then
	  extra_args="$extra_args -v"
       fi
   
       if [[ "$DOWNLOAD_ONLY" == "false" ]]; then
           echo "Testing $image $format $firmware $platform $uri :"
           if [[ "$firmware" == "uefi" ]]; then
               "$CI_SCRIPT" -hypervisor "$HYPERVISOR" -imagetype "$format" -image "$WORKLOADS_DIR"/"$image" -cloudinit "$CLOUD_INIT_DIR" -bios "$WORKLOADS_DIR"/OVMF.fd -platform "$platform" -vsock "$VSOCK" $extra_args  | grep "SUCCESS"
           else
               "$CI_SCRIPT" -hypervisor "$HYPERVISOR" -imagetype "$format" -image "$WORKLOADS_DIR"/"$image" -cloudinit "$CLOUD_INIT_DIR" -platform "$platform" -vsock "$VSOCK" $extra_args | grep "SUCCESS"
           fi
           if [ $? -ne 0 ]; then
             echo "FAILED: Test failed for $image"
           fi
      fi
   done
}

USAGE="Usage: $0 -i vmimage -c cloud-init-dir [--] [qemu options...]
Options:
    -ciscript	FILE	CI Script to be used
    -cloudinit  DIR     Cloud init directory
    -hypervisor FILE    Hypervisor to test
    -workloads  DIR     Directory containing the workload
    -builddir   DIR     Build directory
    -unsafe             Test unsafe images
    -download           Download the workloads. Do not test
    -check              Enable make check. Default false
    -vsock              Enable vsock testing. Default true
    -v                  Verbose mode
    -vv                 -v and verbose tests
    -h , -help		Show this usage information
"

while [ $# -ge 1 ]; do
    case "$1" in
    -ciscript)
        CI_SCRIPT="$2"
        shift 2 ;;
    -cloudinit)
        CLOUD_INIT_DIR="$2"
        shift 2 ;;
    -hypervisor)
        HYPERVISOR="$2"
        shift 2 ;;
    -workloads)
        WORKLOADS_DIR="$2"
        shift 2 ;;
    -unsafe)
        RUN_UNSAFE="true"
        shift;;
    -piix)
        PIIX_TEST="true"
        shift;;
    -download)
        DOWNLOAD_ONLY="true"
        shift;;
    -check)
	CHECK="true"
	shift;;
    -builddir)
        BUILD_DIR="$2"
        shift 2 ;;
    -vsock)
        VSOCK="$2"
        shift 2 ;;
    -v)
        set -x
        shift ;;
    -vv)
        set -x
	VERBOSE="true"
        shift ;;
    -h|-help|--help)
        echo "$USAGE"
        exit ;;
    --)
        shift
        break ;;
    *)
        break ;;
    esac
done

#The array declartion is very finicky. So please watch for the spaces
#Format: image_file_name, format, firmware, platform, extra args, uri
#extra args -s => do not test hotplug
#           -l => enable legacy serial
#                 Boots with serial enabled, grub sets kernel params to ttyS0 not hvc0
declare -a testimages_x86_64_piix
testimages_x86_64_piix=("clear-22180-cloud.img" "qcow2" "uefi" "x86_64_pc" "-s" \
                        "https://download.clearlinux.org/releases/22180/clear/clear-22180-cloud.img.xz" \
                        "CentOS-6-x86_64-GenericCloud-1802.qcow2" "qcow2" "seabios" "x86_64_pc" "-s -l" \
                        "https://cloud.centos.org/centos/6/images/CentOS-6-x86_64-GenericCloud-1802.qcow2" \
                        "coreos_production_openstack_image.img" "qcow2" "seabios" "x86_64_pc" "-s" \
                        "https://stable.release.core-os.net/amd64-usr/current/coreos_production_openstack_image.img.bz2" \
                        "bionic-server-cloudimg-amd64.img" "qcow2" "seabios" "x86_64_pc" "-s -l" \
                        "http://cloud-images.ubuntu.com/bionic/current/bionic-server-cloudimg-amd64.img" \
                        "CentOS-7-x86_64-GenericCloud-1802.qcow2" "qcow2" "seabios" "x86_64_pc" "-s"  \
                        "https://cloud.centos.org/centos/7/images/CentOS-7-x86_64-GenericCloud-1802.qcow2" \
                        "CentOS-Atomic-Host-7-GenericCloud.qcow2" "qcow2" "seabios" "x86_64_pc" "-s" \
                        "http://cloud.centos.org/centos/7/atomic/images/CentOS-Atomic-Host-7-GenericCloud.qcow2" \
                        "Fedora-AtomicHost-28-1.1.x86_64.qcow2" "qcow2" "seabios" "x86_64_pc" "-s" \
                        "https://archives.fedoraproject.org/pub/fedora/linux/releases/28/AtomicHost/x86_64/images/Fedora-AtomicHost-28-1.1.x86_64.qcow2" \
                        "Fedora-Cloud-Base-27-1.6.x86_64.qcow2" "qcow2" "seabios" "x86_64_pc" "-s" \
                        "https://archives.fedoraproject.org/pub/fedora/linux/releases/27/CloudImages/x86_64/images/Fedora-Cloud-Base-27-1.6.x86_64.qcow2" \
                        "Fedora-Cloud-Base-28-1.1.x86_64.qcow2" "qcow2" "seabios" "x86_64_pc" "-s" \
                        "https://archives.fedoraproject.org/pub/fedora/linux/releases/28/Cloud/x86_64/images/Fedora-Cloud-Base-28-1.1.x86_64.qcow2")

declare -a testimages_x86_64_q35
testimages_x86_64_q35=("coreos_production_openstack_image.img" "qcow2" "seabios" "x86_64_q35" "-s" \
                       "https://stable.release.core-os.net/amd64-usr/current/coreos_production_openstack_image.img.bz2" \
                       "CentOS-7-x86_64-GenericCloud-1802.qcow2" "qcow2" "seabios" "x86_64_q35" "-s" \
                       "https://cloud.centos.org/centos/7/images/CentOS-7-x86_64-GenericCloud-1802.qcow2" \
                       "CentOS-Atomic-Host-7-GenericCloud.qcow2" "qcow2" "seabios" "x86_64_q35" "-s" \
                       "http://cloud.centos.org/centos/7/atomic/images/CentOS-Atomic-Host-7-GenericCloud.qcow2" \
                       "Fedora-AtomicHost-28-1.1.x86_64.qcow2" "qcow2" "seabios" "x86_64_q35" "-s" \
                       "https://archives.fedoraproject.org/pub/fedora/linux/releases/28/AtomicHost/x86_64/images/Fedora-AtomicHost-28-1.1.x86_64.qcow2" \
                       "Fedora-Cloud-Base-27-1.6.x86_64.qcow2" "qcow2" "seabios" "x86_64_q35" "-s" \
                       "https://archives.fedoraproject.org/pub/fedora/linux/releases/27/CloudImages/x86_64/images/Fedora-Cloud-Base-27-1.6.x86_64.qcow2" \
                       "Fedora-Cloud-Base-28-1.1.x86_64.qcow2" "qcow2" "seabios" "x86_64_q35" "-s" \
                       "https://archives.fedoraproject.org/pub/fedora/linux/releases/28/Cloud/x86_64/images/Fedora-Cloud-Base-28-1.1.x86_64.qcow2")

# Older unpatched images used to ensure backward compatibility is not broken
# WARNING: These should be run with caution
declare -a unsafeimages_x86_64_piix
unsafeimages_x86_64_piix=("ubuntu-12.04-server-cloudimg-amd64-disk1.img" "qcow2" "seabios" "x86_64_pc" "-s -l" \
                           "http://cloud-images-archive.ubuntu.com/releases/precise/release-20170502/ubuntu-12.04-server-cloudimg-amd64-disk1.img" \
                           "ubuntu-11.10-server-cloudimg-amd64-disk1.img" "qcow2" "seabios" "x86_64_pc" "-s -l" \
                           "http://cloud-images-archive.ubuntu.com/releases/oneiric/release-20130509/ubuntu-11.10-server-cloudimg-amd64-disk1.img" \
                           "core-stable-amd64-disk1.img" "qcow2" "seabios" "x86_64_pc" "-s" \
                           "https://cloud-images.ubuntu.com/snappy/15.04/core/stable/current/core-stable-amd64-disk1.img" \
                           "Fedora-Cloud-Base-20141203-21.x86_64.qcow2" "qcow2" "seabios" "x86_64_pc" "-s -l" \
                           "https://archives.fedoraproject.org/pub/archive/fedora/linux/releases/21/Cloud/Images/x86_64/Fedora-Cloud-Base-20141203-21.x86_64.qcow2")

declare -a unsafeimages_x86_64_q35
unsafeimages_x86_64_q35=("core-stable-amd64-disk1.img" "qcow2" "seabios" "x86_64_q35" "-s" \
                         "https://cloud-images.ubuntu.com/snappy/15.04/core/stable/current/core-stable-amd64-disk1.img")

#Failing images. These fail due to cloud-init issues
#"cirros-0.4.0-x86_64-disk.img" "qcow2" "seabios" \
#"http://download.cirros-cloud.net/0.4.0/cirros-0.4.0-x86_64-disk.img" )
#"debian-9-openstack-amd64.qcow2" "qcow2" "seabios" \
#"http://cdimage.debian.org/cdimage/openstack/current-9/debian-9-openstack-amd64.qcow2" \
#"openSUSE-Leap-42.3-OpenStack.x86_64.qcow2" "qcow2" "seabios
#"https://download.opensuse.org/repositories/Cloud:/Images:/Leap_42.3/images/openSUSE-Leap-42.3-OpenStack.x86_64.qcow2" \


#AARCH64 safe images
declare -a testimages_aarch64
testimages_aarch64=( "bionic-server-cloudimg-arm64.img" "qcow2" "flash" "aarch64" "-s" \
                     "https://cloud-images.ubuntu.com/bionic/current/bionic-server-cloudimg-arm64.img" \
                     "Fedora-Cloud-Base-28-1.1.aarch64.qcow2" "qcow2" "flash" "aarch64" "-s" \
                     "https://archives.fedoraproject.org/pub/fedora/linux/releases/28/Cloud/aarch64/images/Fedora-Cloud-Base-28-1.1.aarch64.qcow2")

if ! [ $(id -u) = 0 ]; then
   die "Needs to be run with effective UID of zero"
fi

if [ ! -d "$WORKLOADS_DIR" ]; then
  mkdir -p "$WORKLOADS_DIR"
  if [ $? -ne 0 ]; then
     die "Unable to create workload directory $WORKLOADS_DIR"
  fi
fi

EFI_FIRMWARE="$WORKLOADS_DIR/OVMF.fd"
if [ ! -f $EFI_FIRMWARE ]; then
   wget --no-verbose -O "$EFI_FIRMWARE" "$EFI_FIRMWARE_URI"
fi

IS_AARCH64=`file $HYPERVISOR | grep aarch64`
if [[ "$BUILD_DIR" == "" ]]; then
    if [[ "$IS_AARCH64" == "" ]]; then
        BUILD_DIR="$HOME/build-x86_64/"
    else
        BUILD_DIR="$HOME/build-aarch64/"
    fi
fi

if [[ "$CHECK" == "true" ]]; then
  echo "Running unit tests"

  extra_check_args=""
  if [[ "$VERBOSE" == "true" ]]; then
      extra_check_args="$extra_check_args V=1"
  fi

  make -C $BUILD_DIR check -j `nproc` $extra_check_args
  if [ $? -ne 0 ]; then
    echo "FAILED: Unit tests"
  fi
fi

if [[ "$RUN_UNSAFE" == "false" ]]; then
    echo "Testing safe images"
    if [[ "$IS_AARCH64" == "" ]]; then
        if [[ "$PIIX_TEST" == "false" ]]; then
            run_tests "${testimages_x86_64_q35[@]}"
        else
            run_tests "${testimages_x86_64_piix[@]}"
            run_tests "${testimages_x86_64_q35[@]}"
        fi
    else
        run_tests "${testimages_aarch64[@]}"
    fi
else
    echo "Testing unsafe images"
    if [[ "$IS_AARCH64" == "" ]]; then
        if [[ "$PIIX_TEST" == "false" ]]; then
            run_tests "${unsafeimages_x86_64_q35[@]}"
        else
            run_tests "${unsafeimages_x86_64_piix[@]}"
            run_tests "${unsafeimages_x86_64_q35[@]}"
        fi
    fi
fi
exit
