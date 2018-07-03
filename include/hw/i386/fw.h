#ifndef QEMU_I386_FW_H
#define QEMU_I386_FW_H

#include "hw/boards.h"

#include "hw/nvram/fw_cfg_keys.h"
#include "hw/timer/hpet.h"

#define FW_CFG_ACPI_TABLES (FW_CFG_ARCH_LOCAL + 0)
#define FW_CFG_SMBIOS_ENTRIES (FW_CFG_ARCH_LOCAL + 1)
#define FW_CFG_IRQ0_OVERRIDE (FW_CFG_ARCH_LOCAL + 2)
#define FW_CFG_E820_TABLE (FW_CFG_ARCH_LOCAL + 3)
#define FW_CFG_HPET (FW_CFG_ARCH_LOCAL + 4)

FWCfgState *fw_cfg_init(uint16_t boot_cpus, const CPUArchIdList *cpus, unsigned apic_id_limit);

#endif
