#!/bin/bash
# -*- mode: shell-script; indent-tabs-mode: nil; sh-basic-offset: 4; -*-
# ex: ts=8 sw=4 sts=4 et filetype=sh
#
#  start_qemu.sh
#
#  Copyright (c) 2016-2017 Intel Corporation

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
VM_NAME="nemuvm"
VM_MEMORY='512'
VM_NCPUS="2"
SSH_PORT=2222
VSOCK="true"
VM_IMAGE_TYPE="raw"
SIMPLE_LAUNCH='false'
CINIT="$SCRIPT_DIR"/cloud-init
PLATFORM='x86_64_pc'
QEMU_PID="qemu.pid"

# Internal variables
debugconsole="false"
legacyserial="false"
vmimage=""
hypervisor="$HOME/build-x86_64/x86_64-softmmu/qemu-system-x86_64"
loop=""
firmware=""
vmimagetmp='testvm.img'
machine='pc'
accel='kvm,kernel_irqchip,nvdimm'
bootindex=1
addr=1
serial=serial.sock
console=console.sock
monitor=monitor.sock
pci_bus="pci.0"

die(){
   echo "${1}"
   exit 1
}

network_setup() {
   # Create a minimal network setup which can be used by vhost
   if [ -x "$(command -v ip)" ]; then
      ip link del testbr &> /dev/null
      ip link del testvlan &> /dev/null
      ip link del testtap &> /dev/null
   
      ip link add name testbr type bridge
      ip link add link testbr name testvlan type macvlan mode bridge
      ip addr add 172.20.0.1/16 dev testvlan
      ip link set dev testvlan up
      ip link set dev testbr up
      ip link add link testbr name testtap type macvtap mode bridge
      ip link set testtap up
   
      tapindex=$(< /sys/class/net/testtap/ifindex)
      tapdev=/dev/tap"$tapindex"
   else
      die $"ip command not found"
   fi
}

ssh_verify() {
   ssh_ip="127.0.0.1"

   retry=0
   max_retries=36
   until [ $retry -ge $max_retries ]
   do 
      ssh_check=$(head -1 < /dev/tcp/"$ssh_ip"/"$SSH_PORT")
      if [[ "$ssh_check" == *SSH-2.0* ]]; then
          echo "SUCCESS: SSH connectivity verified"
          break
      else
          let retry=retry+1
      fi
      sleep 10
   done
   if [ $retry -ge $max_retries ]
   then
	echo "FAILURE: Unable check ssh connectivity into VM"
   fi
}

disk_setup() {
   #Make a local copy of the image
   cp "$vmimage" "$vmimagetmp"

   scsiimg=testscsi.img
   dd if=/dev/zero of=$scsiimg count=1 bs=50M &> /dev/null
   printf "g\nn\n\n\n\nw\n" | fdisk $scsiimg 2&> /dev/null

   nvdimmimg=testnvdimm.img
   dd if=/dev/zero of=$nvdimmimg count=1 bs=50M &> /dev/null
   printf "g\nn\n\n\n\nw\n" | fdisk $nvdimmimg 2&> /dev/null
   nvdimmsize=$(ls -s $nvdimmimg | cut -f1 -d' ')
}

cloudinit_setup() {
   #Create the cloud-init iso
   cloudinitimg="seed.img"
   rm -f "$cloudinitimg"

   truncate --size 2M "$cloudinitimg"
   mkfs.vfat -n cidata "$cloudinitimg" &> /dev/null
   mcopy -oi seed.img  "$CINIT"/user-data  "$CINIT"/meta-data :: 
}

arm_efi_setup() {
    dd if=/dev/zero of=flash0.img bs=1M count=64
    dd if=/usr/share/qemu-efi/QEMU_EFI.fd of=flash0.img conv=notrunc
    dd if=/dev/zero of=flash1.img bs=1M count=64
}

hotplug_check() {
   echo "Hot adding virtio net"
   echo '{ "execute": "qmp_capabilities" }
   { "execute": "device_add", "arguments": { "driver": "virtio-net-pci", "id": "virtio-net1", "bus": "bridge0", "bootindex": "'$bootindex'", "addr": "'$addr'" } }' |
   socat - unix-connect:$monitor
   ((bootindex++))
   ((addr++))

   echo "Hot adding virtio disk"
   virtioimg=test_virtio.img
   # clean up old loops
   ldloops=$(losetup -j $virtioimg | cut -f1 -d:)
   [ -z $oldloops ] || losetup -d $oldloops
   dd if=/dev/zero of=$virtioimg count=1 bs=50M &> /dev/null
   printf "g\nn\n\n\n\nw\n" | fdisk $virtioimg > /dev/null
   losetup -fP $virtioimg
   loop="$(losetup -j $virtioimg | cut -f1 -d:)"
   echo '{ "execute": "qmp_capabilities" }
   { "execute": "blockdev-add", "arguments":{"driver":"raw","node-name":"disk1", "file":{"driver":"file","filename":"'$loop'" } } }
   { "execute": "device_add", "arguments": { "driver": "virtio-blk", "drive": "disk1", "id": "virtio1", "bus": "bridge0", "bootindex": "'$bootindex'", "addr": "'$addr'"} }' |
   socat - unix-connect:$monitor
   ((bootindex++))
   ((addr++))
   
   echo "Hot adding 2 CPUs"
   echo '{ "execute": "qmp_capabilities" }
   { "execute": "query-hotpluggable-cpus" }
   { "execute": "device_add", "arguments": {"driver": "host-x86_64-cpu", "id": "cpu-5", "socket-id": 5, "core-id": 0, "thread-id": 0}}
   { "execute": "device_add", "arguments": {"driver": "host-x86_64-cpu", "id": "cpu-6", "socket-id": 6, "core-id": 0, "thread-id": 0}}' |
   socat - unix-connect:$monitor
}

USAGE="Usage: $0 -i vmimage -c cloud-init-dir [--] [qemu options...]
Options:
    -hypervisor	FILE	Hypervisor binary
    -platform PLATFORM	Platform to run
    -bios  FILE		File containing the BIOS firmware, defaults to seabios
    -image FILE		File containing a raw VM image
    -imagetype IMAGETYPE 	Image type raw/qcow2 [raw]
    -cloudinit DIR      	Cloudinit directory containing cloud-init files
    -memory MEMOR		Memory to use for VM 
    -name NAME		Name to use for VM
    -ssh-port PORT		SSH port to use [2222]
    -vsock [true|false]		Enable vsock
    -s , -simple		Simple bootup, no hotplug
    -d , -debugconsole		Connect to the debug console once the VM is launched
    -l , -legacy                Enable legacy serial support
    -h , -help			Show this usage information
"

while [ $# -ge 1 ]; do
    case "$1" in
    -hypervisor)
        hypervisor="$2"
        shift 2 ;;
    -platform)
        PLATFORM="$2"
        shift 2 ;;
    -bios)
        firmware="$2"
        shift 2 ;;
    -image)
        vmimage="$2"
        shift 2 ;;
    -imagetype)
        VM_IMAGE_TYPE="$2"
        shift 2 ;;
    -cloudinit)
        CINIT="$2"
        shift 2 ;;
    -memory)
        VM_MEMORY="$2"
        shift 2 ;;
    -name)
        VM_NAME="$2"
        shift 2 ;;
    -ssh-port)
        SSH_PORT="$2"
        shift 2 ;;
    -vsock)
        VSOCK="$2"
        shift 2 ;;
    -s|-simple)
        SIMPLE_LAUNCH='true'
        shift ;;
    -d|-debugconsole)
        debugconsole='true'
        shift ;;
    -l|-legacyserial)
        legacyserial='true'
        shift ;;
    -v|-verbose)
        set -x
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

if ! [ $(id -u) = 0 ]; then
   die "Needs to be run with effective UID of zero"
fi

if [ ! -f "$vmimage" ]; then
   die $"Can't find image file $vmimage"
fi


case "$PLATFORM" in
    x86_64_pc)
       machine='pc'
       accel='kvm,kernel_irqchip,nvdimm'
       ;;
    x86_64_q35)
       machine='q35'
       accel='kvm,kernel_irqchip,nvdimm'
       pci_bus='pcie.0'
       SIMPLE_LAUNCH="true"
       ;;
    x86_64_virt)
       machine='virt'
       accel='kvm,kernel_irqchip'
       SIMPLE_LAUNCH="true"
       ;;
    aarch64)
       machine='virt,gic-version=host'
       accel='kvm'
       ;;
    *)
        die $"Invalid platform type \"$PLATFORM\""
esac

if [ ! -f "$hypervisor" ]; then
    die $"$hypervisor not found"
fi

if [ -f "$QEMU_PID" ]; then
   #Cleanup any existing instances
   kill -9 $(cat $QEMU_PID) &> /dev/null
fi

cloudinit_setup
network_setup
disk_setup

if [ "$PLATFORM" = "aarch64" ]; then
   arm_efi_setup
   echo "Forcing simple launch on ARM64"
   pci_bus="pcie.0"
   SIMPLE_LAUNCH="true"
fi


qemu_args=" -machine $machine,accel=$accel"

qemu_args+=" -pidfile $QEMU_PID"
qemu_args+=" -qmp unix:$monitor,server,nowait"

if [ "$firmware" != "" ]; then
      if [ ! -f "$firmware" ]; then
         die $"firmware not found"
      fi
      qemu_args+=" -bios $firmware"
fi

qemu_args+=" -smp $VM_NCPUS,cores=1,threads=1,sockets=$VM_NCPUS,maxcpus=32" 
qemu_args+=" -m $VM_MEMORY,slots=4,maxmem=16384M"
qemu_args+=" -cpu host -nographic -no-user-config"
qemu_args+=" -nodefaults"
qemu_args+=" -daemonize"

if [ "$PLATFORM" != "aarch64" ]; then

if [ "$PLATFORM" != "x86_64_virt" ]; then
   qemu_args+=" -device pci-bridge,bus=$pci_bus,id=bridge0,addr=2,chassis_nr=1,shpc=on"
fi

   qemu_args+=" -drive file=$vmimagetmp,if=none,id=drive-virtio-disk0,format=$VM_IMAGE_TYPE \
         -device virtio-blk-pci,scsi=off,drive=drive-virtio-disk0,id=virtio-disk0,bootindex=$bootindex"
   ((bootindex++))

if [ "$PLATFORM" != "x86_64_virt" ]; then
   qemu_args+=" -drive file=$cloudinitimg,if=virtio,media=cdrom"
else
   qemu_args+=" -device sysbus-debugcon,iobase=0x402,chardev=debugcon -chardev file,path=/tmp/debug-log,id=debugcon"
   qemu_args+=" -device virtio-blk-pci,drive=cloud \
             -drive if=none,id=cloud,file=$cloudinitimg,format=raw"
fi

   qemu_args+=" -netdev user,id=mynet0,hostfwd=tcp::$SSH_PORT-:22,hostname=$VM_NAME \
         -device virtio-net-pci,netdev=mynet0"
   
   qemu_args+=" -drive file=$scsiimg,if=none,id=drive-virtio-disk1,format=raw \
         -device virtio-scsi-pci,id=virtio-disk1"

   qemu_args+=" -object memory-backend-file,id=mem0,share,mem-path=$nvdimmimg,size=$nvdimmsize \
         -device nvdimm,memdev=mem0,id=nv0"
   # Our ARM test system doesn't have CONFIG_VHOST_VSOCK
   if [[ "$VSOCK" == "true" ]]; then
       qemu_args+=" -device vhost-vsock-pci,id=vhost-vsock-pci0,guest-cid=3"
   fi
else
   qemu_args+=" \
    -drive if=pflash,file=flash0.img,format=raw \
    -drive if=pflash,file=flash1.img,format=raw"
    qemu_args+=" -device pci-bridge,bus=pcie.0,id=pci-bridge-0,chassis_nr=1,shpc=on "

    qemu_args+=" -drive if=none,file=$vmimagetmp,id=hd0 \
             -device virtio-blk-device,drive=hd0 "

    # No CDROM for virt machine
    qemu_args+=" -device virtio-blk-device,drive=cloud \
             -drive if=none,id=cloud,file=$cloudinitimg"

    # Use virtio-net-device not virtio-net-pci
    qemu_args+=" -netdev user,id=mynet0,hostfwd=tcp::$SSH_PORT-:22,hostname=$VM_NAME \
             -device virtio-net-device,netdev=mynet0"
fi

qemu_args+=" -device virtio-serial-pci,id=virtio-serial0 \
         -device virtconsole,chardev=charconsole0,id=console0 \
         -chardev socket,id=charconsole0,path=$console,server,nowait"

exec 3<>$tapdev
exec 4<>/dev/vhost-net
mac=$(< /sys/class/net/testtap/address)
qemu_args+=" -netdev tap,fd=3,id=hostnet0,vhost=on,vhostfd=4 \
         -device virtio-net-pci,netdev=hostnet0,id=net0,mac=$mac"


qemu_args+=" -device virtio-rng-pci,rng=rng0 \
         -object rng-random,filename=/dev/random,id=rng0"

qemu_args+=" -device virtio-balloon-pci"
qemu_args+=" -object cryptodev-backend-builtin,id=cryptodev0"
qemu_args+=" -device virtio-crypto-pci,id=crypto0,cryptodev=cryptodev0"

if [ "$legacyserial" = "true" ]; then
   qemu_args+=" -device isa-serial,chardev=serialconsole0,id=serial0 \
                -chardev socket,id=serialconsole0,path=$serial,server,nowait"
fi


# Launch with all virtio devices
$hypervisor $qemu_args "$@" || exit

if [ "$SIMPLE_LAUNCH" = "false" ]; then
   hotplug_check
fi

ssh_verify

#Open a console
if [ "$debugconsole" = "true" ]; then
    socat stdin,raw,echo=0,escape=0x11 unix-connect:$console
fi

#TODO: Cleanly shutdown the VM via QMP
kill -9 $(cat $QEMU_PID)

if [ "$SIMPLE_LAUNCH" = "false" ]; then
   losetup -d $loop
fi
