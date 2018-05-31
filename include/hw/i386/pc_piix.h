#ifndef PC_PIIX_H
#define PC_PIIX_H

#include "hw/isa/isa.h"
#include "sysemu/sysemu.h"
#include "hw/pci/pci.h"
#include "hw/acpi/aml-build.h"


#ifdef CONFIG_PIIX

struct PCII440FXState;
typedef struct PCII440FXState PCII440FXState;

#define TYPE_I440FX_PCI_HOST_BRIDGE "i440FX-pcihost"
#define TYPE_I440FX_PCI_DEVICE "i440FX"

PCIBus *i440fx_init(const char *host_type, const char *pci_type,
                PCII440FXState **pi440fx_state, int *piix_devfn,
                ISABus **isa_bus, qemu_irq *pic,
                MemoryRegion *address_space_mem,
                MemoryRegion *address_space_io,
                ram_addr_t ram_size,
                ram_addr_t below_4g_mem_size,
                ram_addr_t above_4g_mem_size,
                MemoryRegion *pci_memory,
                MemoryRegion *ram_memory);

PCIBus *find_i440fx(void);
/* piix4.c */
extern PCIDevice *piix4_dev;
int piix4_init(PCIBus *bus, ISABus **isa_bus, int devfn);

#endif

/*acpi-build-piix.c */
void build_append_pcihp_notify_entry(Aml *method, int slot);
void build_piix4_pm(Aml *table);
void build_piix4_isa_bridge(Aml *table);
void build_piix4_pci_hotplug(Aml *table);
void build_piix4_pci0_int(Aml *table);

#endif
