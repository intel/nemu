#ifndef QEMU_I386_FW_H
#define QEMU_I386_FW_H

#include "hw/boards.h"

#include "hw/nvram/fw_cfg.h"

#include "hw/timer/hpet.h"

#define FW_CFG_ACPI_TABLES (FW_CFG_ARCH_LOCAL + 0)
#define FW_CFG_SMBIOS_ENTRIES (FW_CFG_ARCH_LOCAL + 1)
#define FW_CFG_IRQ0_OVERRIDE (FW_CFG_ARCH_LOCAL + 2)
#define FW_CFG_E820_TABLE (FW_CFG_ARCH_LOCAL + 3)
#define FW_CFG_HPET (FW_CFG_ARCH_LOCAL + 4)

#define FW_CFG_IO_BASE 0x510

/* fw_cfg machine ids */
enum {
    X86_I440FX = 1,
    X86_Q35,
    X86_ISAPC,
    X86_XENFV,
    X86_XENPV,
    X86_VIRT,
};

FWCfgState *fw_cfg_init(MachineState *ms, uint16_t boot_cpus, const CPUArchIdList *cpus, unsigned apic_id_limit);
void pc_system_rom_init(MemoryRegion *rom_memory, bool rw_fw);

#endif
