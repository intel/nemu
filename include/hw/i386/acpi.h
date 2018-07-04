#ifndef QEMU_I386_ACPI_H
#define QEMU_I386_ACPI_H

#include "qemu-common.h"

#include "hw/hotplug.h"
#include "hw/mem/pc-dimm.h"
#include "hw/mem/nvdimm.h"
#include "hw/nvram/fw_cfg.h"

typedef
struct AcpiBuildState {
    /* Copy of table in RAM (for patching). */
    MemoryRegion *table_mr;
    /* Is table patched? */
    uint8_t patched;
    void *rsdp;
    MemoryRegion *rsdp_mr;
    MemoryRegion *linker_mr;
} AcpiBuildState;


typedef
struct AcpiConfiguration {
    /* Machine class settings */
    int legacy_acpi_table_size;
    bool legacy_cpu_hotplug;
    bool rsdp_in_ram;
    unsigned acpi_data_size;
    bool linuxboot_dma_enabled;

    /* Machine state settings */
    FWCfgState *fw_cfg;
    HotplugHandler *acpi_dev;
    ram_addr_t below_4g_mem_size;
    uint64_t numa_nodes;
    uint64_t *node_mem;
    bool apic_xrupt_override;
    unsigned apic_id_limit;
    AcpiNVDIMMState acpi_nvdimm_state;
    MemoryHotplugState hotplug_memory;

    /* Build state */
    AcpiBuildState *build_state;
} AcpiConfiguration;


#endif
