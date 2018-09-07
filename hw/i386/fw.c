/*
 *
 * Copyright (c) 2018 Intel Corportation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "sysemu/numa.h"
#include "exec/cpu-common.h"

#include "hw/acpi/acpi.h"
#include "hw/timer/hpet.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/smbios/smbios.h"

#include "hw/i386/fw.h"
#include "hw/i386/memory.h"
#include "hw/i386/pc.h"

#include "kvm_i386.h"

struct hpet_fw_config hpet_cfg = {.count = UINT8_MAX};

static void fw_build_feature_control_file(MachineState *ms, FWCfgState *fw_cfg)
{
    X86CPU *cpu = X86_CPU(ms->possible_cpus->cpus[0].cpu);
    CPUX86State *env = &cpu->env;
    uint32_t unused, ecx, edx;
    uint64_t feature_control_bits = 0;
    uint64_t *val;

    cpu_x86_cpuid(env, 1, 0, &unused, &unused, &ecx, &edx);
    if (ecx & CPUID_EXT_VMX) {
        feature_control_bits |= FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX;
    }

    if ((edx & (CPUID_EXT2_MCE | CPUID_EXT2_MCA)) ==
        (CPUID_EXT2_MCE | CPUID_EXT2_MCA) &&
        (env->mcg_cap & MCG_LMCE_P)) {
        feature_control_bits |= FEATURE_CONTROL_LMCE;
    }

    if (!feature_control_bits) {
        return;
    }

    val = g_malloc(sizeof(*val));
    *val = cpu_to_le64(feature_control_bits | FEATURE_CONTROL_LOCKED);
    fw_cfg_add_file(fw_cfg, "etc/msr_feature_control", val, sizeof(*val));
}


FWCfgState *fw_cfg_init(MachineState *ms, uint16_t boot_cpus, const CPUArchIdList *cpus, unsigned apic_id_limit)
{
    FWCfgState *fw_cfg;
    uint64_t *numa_fw_cfg;
    int i;

    fw_cfg = fw_cfg_init_io_dma(FW_CFG_IO_BASE, FW_CFG_IO_BASE + 4, &address_space_memory);
    fw_cfg_add_i16(fw_cfg, FW_CFG_NB_CPUS, boot_cpus);

    /* FW_CFG_MAX_CPUS is a bit confusing/problematic on x86:
     *
     * For machine types prior to 1.8, SeaBIOS needs FW_CFG_MAX_CPUS for
     * building MPTable, ACPI MADT, ACPI CPU hotplug and ACPI SRAT table,
     * that tables are based on xAPIC ID and QEMU<->SeaBIOS interface
     * for CPU hotplug also uses APIC ID and not "CPU index".
     * This means that FW_CFG_MAX_CPUS is not the "maximum number of CPUs",
     * but the "limit to the APIC ID values SeaBIOS may see".
     *
     * So for compatibility reasons with old BIOSes we are stuck with
     * "etc/max-cpus" actually being apic_id_limit
     */
    fw_cfg_add_i16(fw_cfg, FW_CFG_MAX_CPUS, (uint16_t)apic_id_limit);
    fw_cfg_add_i64(fw_cfg, FW_CFG_RAM_SIZE, (uint64_t)ram_size);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_ACPI_TABLES,
                     acpi_tables, acpi_tables_len);
    fw_cfg_add_i32(fw_cfg, FW_CFG_IRQ0_OVERRIDE, kvm_allows_irq0_override());

    fw_cfg_add_bytes(fw_cfg, FW_CFG_E820_TABLE,
                     &e820_reserve, sizeof(e820_reserve));
    fw_cfg_add_file(fw_cfg, "etc/e820", e820_table,
                    sizeof(struct e820_entry) * e820_entries);

    fw_cfg_add_bytes(fw_cfg, FW_CFG_HPET, &hpet_cfg, sizeof(hpet_cfg));
    /* allocate memory for the NUMA channel: one (64bit) word for the number
     * of nodes, one word for each VCPU->node and one word for each node to
     * hold the amount of memory.
     */
    numa_fw_cfg = g_new0(uint64_t, 1 + apic_id_limit + nb_numa_nodes);
    numa_fw_cfg[0] = cpu_to_le64(nb_numa_nodes);
    for (i = 0; i < cpus->len; i++) {
        unsigned int apic_id = cpus->cpus[i].arch_id;
        assert(apic_id < apic_id_limit);
        numa_fw_cfg[apic_id + 1] = cpu_to_le64(cpus->cpus[i].props.node_id);
    }
    for (i = 0; i < nb_numa_nodes; i++) {
        numa_fw_cfg[apic_id_limit + 1 + i] =
            cpu_to_le64(numa_info[i].node_mem);
    }
    fw_cfg_add_bytes(fw_cfg, FW_CFG_NUMA, numa_fw_cfg,
                     (1 + apic_id_limit + nb_numa_nodes) *
                     sizeof(*numa_fw_cfg));

    /* SMBIOS */
    fw_build_smbios(ms, fw_cfg);

    /* MSR features control */
    fw_build_feature_control_file(ms, fw_cfg);

    return fw_cfg;
}
