# Default configuration for riscv-softmmu

include hw-common.mak
CONFIG_SERIAL=y
CONFIG_VIRTIO_MMIO=y
include virtio.mak

CONFIG_CADENCE=y
