#!/bin/sh
# enable automatic i386/ARM
# program execution by the kernel

qemu_target_list="i386 i486 arm armeb \
aarch64 aarch64_be"

i386_magic='\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x03\x00'
i386_mask='\xff\xff\xff\xff\xff\xfe\xfe\xff\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'
i386_family=i386

i486_magic='\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x06\x00'
i486_mask='\xff\xff\xff\xff\xff\xfe\xfe\xff\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'
i486_family=i386

arm_magic='\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00'
arm_mask='\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'
arm_family=arm

armeb_magic='\x7fELF\x01\x02\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28'
armeb_mask='\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff'
armeb_family=armeb

aarch64_magic='\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00'
aarch64_mask='\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'
aarch64_family=arm

aarch64_be_magic='\x7fELF\x02\x02\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7'
aarch64_be_mask='\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff'
aarch64_be_family=armeb

qemu_get_family() {
    cpu=${HOST_ARCH:-$(uname -m)}
    case "$cpu" in
    amd64|i386|i486|i586|i686|i86pc|BePC|x86_64)
        echo "i386"
        ;;
    arm|armel|armhf|arm64|armv[4-9]*l|aarch64)
        echo "arm"
        ;;
    armeb|armv[4-9]*b|aarch64_be)
        echo "armeb"
        ;;
    *)
        echo "$cpu"
        ;;
    esac
}

usage() {
    cat <<EOF
Usage: qemu-binfmt-conf.sh [--qemu-path PATH][--debian][--systemd CPU]
                           [--help][--credential yes|no][--exportdir PATH]

       Configure binfmt_misc to use qemu interpreter

       --help:       display this usage
       --qemu-path:  set path to qemu interpreter ($QEMU_PATH)
       --debian:     don't write into /proc,
                     instead generate update-binfmts templates
       --systemd:    don't write into /proc,
                     instead generate file for systemd-binfmt.service
                     for the given CPU. If CPU is "ALL", generate a
                     file for all known cpus
       --exportdir:  define where to write configuration files
                     (default: $SYSTEMDDIR or $DEBIANDIR)
       --credential: if yes, credential and security tokens are
                     calculated according to the binary to interpret

    To import templates with update-binfmts, use :

        sudo update-binfmts --importdir ${EXPORTDIR:-$DEBIANDIR} --import qemu-CPU

    To remove interpreter, use :

        sudo update-binfmts --package qemu-CPU --remove qemu-CPU $QEMU_PATH

    With systemd, binfmt files are loaded by systemd-binfmt.service

    The environment variable HOST_ARCH allows to override 'uname' to generate
    configuration files for a different architecture than the current one.

    where CPU is one of:

        $qemu_target_list

EOF
}

qemu_check_access() {
    if [ ! -w "$1" ] ; then
        echo "ERROR: cannot write to $1" 1>&2
        exit 1
    fi
}

qemu_check_bintfmt_misc() {
    # load the binfmt_misc module
    if [ ! -d /proc/sys/fs/binfmt_misc ]; then
      if ! /sbin/modprobe binfmt_misc ; then
          exit 1
      fi
    fi
    if [ ! -f /proc/sys/fs/binfmt_misc/register ]; then
      if ! mount binfmt_misc -t binfmt_misc /proc/sys/fs/binfmt_misc ; then
          exit 1
      fi
    fi

    qemu_check_access /proc/sys/fs/binfmt_misc/register
}

installed_dpkg() {
    dpkg --status "$1" > /dev/null 2>&1
}

qemu_check_debian() {
    if [ ! -e /etc/debian_version ] ; then
        echo "WARNING: your system is not a Debian based distro" 1>&2
    elif ! installed_dpkg binfmt-support ; then
        echo "WARNING: package binfmt-support is needed" 1>&2
    fi
    qemu_check_access "$EXPORTDIR"
}

qemu_check_systemd() {
    if ! systemctl -q is-enabled systemd-binfmt.service ; then
        echo "WARNING: systemd-binfmt.service is missing or disabled" 1>&2
    fi
    qemu_check_access "$EXPORTDIR"
}

qemu_generate_register() {
    echo ":qemu-$cpu:M::$magic:$mask:$qemu:$FLAGS"
}

qemu_register_interpreter() {
    echo "Setting $qemu as binfmt interpreter for $cpu"
    qemu_generate_register > /proc/sys/fs/binfmt_misc/register
}

qemu_generate_systemd() {
    echo "Setting $qemu as binfmt interpreter for $cpu for systemd-binfmt.service"
    qemu_generate_register > "$EXPORTDIR/qemu-$cpu.conf"
}

qemu_generate_debian() {
    cat > "$EXPORTDIR/qemu-$cpu" <<EOF
package qemu-$cpu
interpreter $qemu
magic $magic
mask $mask
EOF
    if [ "$FLAGS" = "OC" ] ; then
        echo "credentials yes" >> "$EXPORTDIR/qemu-$cpu"
    fi
}

qemu_set_binfmts() {
    # probe cpu type
    host_family=$(qemu_get_family)

    # register the interpreter for each cpu except for the native one

    for cpu in ${qemu_target_list} ; do
        magic=$(eval echo \$${cpu}_magic)
        mask=$(eval echo \$${cpu}_mask)
        family=$(eval echo \$${cpu}_family)

        if [ "$magic" = "" ] || [ "$mask" = "" ] || [ "$family" = "" ] ; then
            echo "INTERNAL ERROR: unknown cpu $cpu" 1>&2
            continue
        fi

        qemu="$QEMU_PATH/qemu-$cpu"
        if [ "$cpu" = "i486" ] ; then
            qemu="$QEMU_PATH/qemu-i386"
        fi

        if [ "$host_family" != "$family" ] ; then
            $BINFMT_SET
        fi
    done
}

CHECK=qemu_check_bintfmt_misc
BINFMT_SET=qemu_register_interpreter

SYSTEMDDIR="/etc/binfmt.d"
DEBIANDIR="/usr/share/binfmts"

QEMU_PATH=/usr/local/bin
FLAGS=""

options=$(getopt -o ds:Q:e:hc: -l debian,systemd:,qemu-path:,exportdir:,help,credential: -- "$@")
eval set -- "$options"

while true ; do
    case "$1" in
    -d|--debian)
        CHECK=qemu_check_debian
        BINFMT_SET=qemu_generate_debian
        EXPORTDIR=${EXPORTDIR:-$DEBIANDIR}
        ;;
    -s|--systemd)
        CHECK=qemu_check_systemd
        BINFMT_SET=qemu_generate_systemd
        EXPORTDIR=${EXPORTDIR:-$SYSTEMDDIR}
        shift
        # check given cpu is in the supported CPU list
        if [ "$1" != "ALL" ] ; then
            for cpu in ${qemu_target_list} ; do
                if [ "$cpu" = "$1" ] ; then
                    break
                fi
            done

            if [ "$cpu" = "$1" ] ; then
                qemu_target_list="$1"
            else
                echo "ERROR: unknown CPU \"$1\"" 1>&2
                usage
                exit 1
            fi
        fi
        ;;
    -Q|--qemu-path)
        shift
        QEMU_PATH="$1"
        ;;
    -e|--exportdir)
        shift
        EXPORTDIR="$1"
        ;;
    -h|--help)
        usage
        exit 1
        ;;
    -c|--credential)
        shift
        if [ "$1" = "yes" ] ; then
            FLAGS="OC"
        else
            FLAGS=""
        fi
        ;;
    *)
        break
        ;;
    esac
    shift
done

$CHECK
qemu_set_binfmts
