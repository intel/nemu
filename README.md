- [NEMU, a cloud hypervisor](#nemu--a-cloud-hypervisor)
  * [Rationale](#rationale)
  * [QEMU base](#qemu-base)
  * [*NEMU*](#-nemu-)
  * [Requirements](#requirements)
    + [High Level](#high-level)
    + [Architectures](#architectures)
    + [Guest OS](#guest-os)
    + [Guest Platforms](#guest-platforms)
    + [Host Platforms](#host-platforms)
    + [Firmware and boot](#firmware-and-boot)
    + [Boot methods](#boot-methods)
    + [Memory](#memory)
    + [Devices](#devices)
      - [Models](#models)
      - [Block](#block)
    + [Guest Image Formats](#guest-image-formats)
    + [Migration](#migration)
    + [Monitoring](#monitoring)
    + [**To be discussed**](#--to-be-discussed--)


# NEMU, a cloud hypervisor

*NEMU* is an open source hypervisor specifically built and designed to run modern cloud
workloads on modern 64-bit Intel and ARM CPUs.

## Rationale

Modern guest operating systems that host cloud workloads run on virtual hardware platforms
that do not require any legacy hardware. Additonally modern CPUs used in data centers have
advanced virtualization features that have eliminated the need for most CPU emulation.

There currently is no open source hypervisor solutions with a clear and narrow focus on
running cloud specific workloads on modern CPUs. All available solutions have evolved over
time and try to be fairly generic. They attempt to support a wide range of virtual hardware
architectures and run on hardware that has varying degree of hardware virtualization support.
This results in a need to provide a large set of legacy platforms and device models requiring
CPU, device and platform emulation. As a consequence they are built on top of large and
complex code bases.

*NEMU* on the other hand aims to leverage KVM, be narrow focused on exclusively running modern,
cloud native workloads, on top of a limited set of hardware architectures and platforms.
It assumes fairly recent CPUs and KVM allowing for the the elimination of most emulation logic.

This will allow for smaller code base, lower complexity and a reduced attack surface compared
to existing solutions. It also gives more space for providing cloud specific optimizations and
building a more performant hypervisor for the cloud. Reducing the size and complexity of the code
allows for easier review, fuzz testing, modularization and future innovation.

## QEMU base

QEMU is the current de facto standard open source cloud hypervisor. It has a rich set of features
that have been developed and tested over time. This includes features such as live migration,
PCI, Memory, NVDIMM and CPU hotplug, VFIO, mediated device passthrough and vhost-user. QEMU also
has been the code base on which significant effort and innovation has been invested to create multiple
performant I/O models

It also comes with a very large support for legacy features, for platforms and devices and is capable
of running on a large number of hardware platforms. It also allows for cross platform emulation.
One of its fundamental goal is about being as generic as possible and run on a large set of hardware
and host a diversity of workloads. QEMU needed emulation support to be build into the code as hardware
lacked critical virtualization features.

QEMU allows for build time configuration of some of its rich feature set. However there is quite a
large amount of the code base that cannot be compiled out as the emulated platforms make assumptions
about certain legacy devices being always present. QEMU also has abstractions within the code to support
all of these legacy features.

## *NEMU*

*NEMU* is based off QEMU and leverage its rich feature set, but with a much narrower focus.
It leverages the performant, robust and stable QEMU codebase without the need to supporting the
myriad of features, platforms and harware that are not relevant for the cloud.

The goal of *NEMU* is to retain the absolute minimal subset of the QEMU codebase that is required
for the feature set described below. The QEMU code base will also be simplified to reduce the number
of generic abstractions.

## Requirements

*NEMU* provides a PCI virtio platform with support for vfio based device direct assigment and mediated
device assigment support. It also aims to retain support for live migration, vhost-user and a build time
configurable device hotplug support for PCI, memory, NVDIMM and CPU. *NEMU* will need to emulate a small
subset of features including PCI host brige.

*NEMU* also introduces a new QEMU x86-64 machine type: **`virt`**. It is a purely virtual platform, that
does not try to emulate any existing x86 chipset or legacy bus (ISA, SMBUS, etc) and offloads as many features
to KVM as possible. This is a similar approach as the already existin AArch64 `virt` machine type and *NEMU*
will only support the two `virt` machine types.

Below is a list of QEMU features that *NEMU* will retain and add.

### High Level

* KVM and KVM only based
* Minimal emulation
* Low latency
* Low memory footprint
* Low complexity
* Small attack surface
* 64-bit support only
* Optional and build time configurable CPU, memory, PCI and NVDIMM hotplug
* Machine to machine migration

### Architectures

*NEMU* only supports two 64-bit CPU architectures:
* `x86-64`
* `AArch64`

### Guest OS
* `64-bit Linux`

### Guest Platforms

* `virt` (x86-64) QEMU x86-64 virtual machine
* `virt` (AArch64) QEMU AArch64 virtual machine

### Host Platforms

* `Linux`

### Firmware and boot

* `UEFI`
* `ACPI`
  * Hardware Reduced ACPI
  * Optional hotplug support
	* CPU
	* Memory
	* NVDIMM
	* PCI devices
	* VFIO
	* vhost-user

### Boot methods

* `UEFI boot`

### Memory

* `QEMU allocated memory`
* `File mapped memory`
* `Huge pages`
* `Memory pinning`

### Devices

#### Models

* `virtio`
  * `blk`
  * `console`
  * `crypto`
  * `pci-net`
  * `rng-pci`
  * `scsi`
	* `virtio`
	* `vhost`
  * `9pfs`
  * `vhost-user-scsi`
  * `vhost-user-net`
  * `vhost-user-blk`
  * `vhost-vsock-pci`
* `vfio`
  * `network`
  * `mediated device`
  * `storage`
  * `rdma`
* `NVDIMM`
* `TPM`
  * `vTPM`
  * `Host TPM passthrough`
* `SCSI controller`
* `PCI controller (pci-lite)`

#### Block

* `cdrom`
* `nvme`
* `ceph/rbd`

### Guest Image Formats

* `QCOW2`
* `RAW`
* `VHD`

### Migration

* `Network based over TLS`
* `File based` (Local migration)

### Monitoring

* `QMP`
* `QAPI`

### **To be discussed**

* `64-bit Windows Server (headless)`
* `qboot`
* `Graphic Console`
* `virtio-block-crypto`
* `QEMU client support as modules`
  * `iscsi`
  * `nbd`
  * `nfs`
  * `gluster`
* `RDMA live migration`
* `SLIRP`
* `Guest agent`
