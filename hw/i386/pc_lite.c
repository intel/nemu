/*
 * Light weight PC chipset
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 * Copyright (c) 2009, 2010
 *               Isaku Yamahata <yamahata at valinux co jp>
 *               VA Linux Systems Japan K.K.
 * Copyright (C) 2012 Jason Baron <jbaron@redhat.com>
 * Copyright (C) 2016 Chao Peng <chao.p.peng@linux.intel.com>
 *
 * This is based on pc_q35.c, but heavily modified.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/loader.h"
#include "sysemu/arch_init.h"
#include "hw/boards.h"
#include "sysemu/kvm.h"
#include "hw/kvm/clock.h"
#include "hw/pci-host/q35.h"
#include "exec/address-spaces.h"
#include "hw/i386/ich9.h"
#include "hw/smbios/smbios.h"
#include "qemu/error-report.h"
#include "migration/migration.h"
#include "qapi/error.h"

static void pc_lite_gsi_handler(void *opaque, int n, int level)
{
    GSIState *s = opaque;

    qemu_set_irq(s->ioapic_irq[n], level);
}

static void pc_lite_init(MachineState *machine)
{
    PCMachineState *pcms = PC_MACHINE(machine);
    PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);
    PCIBus *host_bus;
    MemoryRegion *pci_memory;
    MemoryRegion *rom_memory;
    MemoryRegion *ram_memory;
    GSIState *gsi_state;
    qemu_irq *gsi;
    ram_addr_t lowmem;
    DeviceState *pm;
    MachineClass *mc = MACHINE_GET_CLASS(machine);

    /* Check whether RAM fits below 4G (leaving 1/2 GByte for IO memory
     * and 256 Mbytes for PCI Express Enhanced Configuration Access Mapping
     * also known as MMCFG).
     * If it doesn't, we need to split it in chunks below and above 4G.
     * In any case, try to make sure that guest addresses aligned at
     * 1G boundaries get mapped to host addresses aligned at 1G boundaries.
     */
    if (machine->ram_size >= 0xb0000000) {
        lowmem = 0x80000000;
    } else {
        lowmem = 0xb0000000;
    }

    /* Handle the machine opt max-ram-below-4g.  It is basically doing
     * min(qemu limit, user limit).
     */
    if (lowmem > pcms->max_ram_below_4g) {
        lowmem = pcms->max_ram_below_4g;
        if (machine->ram_size - lowmem > lowmem &&
            lowmem & ((1ULL << 30) - 1)) {
            error_report("Warning: Large machine and max_ram_below_4g(%"PRIu64
                         ") not a multiple of 1G; possible bad performance.",
                         pcms->max_ram_below_4g);
        }
    }

    if (machine->ram_size >= lowmem) {
        pcms->above_4g_mem_size = machine->ram_size - lowmem;
        pcms->below_4g_mem_size = lowmem;
    } else {
        pcms->above_4g_mem_size = 0;
        pcms->below_4g_mem_size = machine->ram_size;
    }

    pc_cpus_init(pcms);

    kvmclock_create();

    /* pci enabled */
    if (pcmc->pci_enabled) {
        pci_memory = g_new(MemoryRegion, 1);
        memory_region_init(pci_memory, NULL, "pci", UINT64_MAX);
        rom_memory = pci_memory;
    } else {
        pci_memory = NULL;
        rom_memory = get_system_memory();
    }

    pc_guest_info_init(pcms);

    if (pcmc->smbios_defaults) {
        /* These values are guest ABI, do not change */
        smbios_set_defaults("QEMU", "Light weight PC)",
                            mc->name, pcmc->smbios_legacy_mode,
                            pcmc->smbios_uuid_encoded,
                            SMBIOS_ENTRY_POINT_21);
    }

    /* allocate ram and load rom/bios */
    pc_memory_init(pcms, get_system_memory(),
                   rom_memory, &ram_memory);

    /* irq lines */
    gsi_state = g_malloc0(sizeof(*gsi_state));
    if (kvm_irqchip_in_kernel()) {
        kvm_pc_setup_irq_routing(pcmc->pci_enabled);
    }
    gsi = qemu_allocate_irqs(pc_lite_gsi_handler, gsi_state, GSI_NUM_PINS);

    if (pcmc->pci_enabled) {
        host_bus = pci_lite_init(get_system_memory(), get_system_io(),
                                     pci_memory);
        pcms->bus = host_bus;

        if (acpi_enabled) {
            pm = pm_lite_init(host_bus, -1, gsi[9]);

            object_property_add_link(OBJECT(machine),
                                     PC_MACHINE_ACPI_DEVICE_PROP,
                                     TYPE_HOTPLUG_HANDLER,
                                     (Object **)&pcms->acpi_dev,
                                     object_property_allow_set_link,
                                     OBJ_PROP_LINK_UNREF_ON_RELEASE,
                                     &error_abort);
            object_property_set_link(OBJECT(machine), OBJECT(pm),
                                     PC_MACHINE_ACPI_DEVICE_PROP, &error_abort);
        }
    }

    if (pcmc->pci_enabled) {
        ioapic_init_gsi(gsi_state, "pcilite");
    }

    pc_register_ferr_irq(gsi[13]);

    if (pcms->acpi_nvdimm_state.is_enabled) {
        nvdimm_init_acpi_state(&pcms->acpi_nvdimm_state, get_system_io(),
                               pcms->fw_cfg, OBJECT(pcms));
    }
}

#define DEFINE_LITE_MACHINE(suffix, name, compatfn, optionfn) \
    static void pc_init_##suffix(MachineState *machine) \
    { \
        void (*compat)(MachineState *m) = (compatfn); \
        if (compat) { \
            compat(machine); \
        } \
        pc_lite_init(machine); \
    } \
    DEFINE_PC_MACHINE(suffix, name, pc_init_##suffix, optionfn)


static void pc_lite_machine_options(MachineClass *m)
{
    m->family = "pc_lite";
    m->desc = "Light weight PC";
    m->hot_add_cpu = pc_hot_add_cpu;
    m->units_per_default_bus = 1;
    m->no_floppy = 1;
}

static void pc_lite_2_12_machine_options(MachineClass *m)
{
    pc_lite_machine_options(m);
    m->alias = "pc-lite";
}

DEFINE_LITE_MACHINE(v2_12, "pc-lite-2.12", NULL,
                    pc_lite_2_12_machine_options);
