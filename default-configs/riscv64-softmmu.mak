# Default configuration for riscv-softmmu

CONFIG_SERIAL=y
CONFIG_VIRTIO_MMIO=y
include virtio.mak

CONFIG_CADENCE=y
CONFIG_USB_CORE=$(CONFIG_USB_REDIR)
