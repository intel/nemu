/* HW reduced ACPI support
 *
 * Copyright (c) 2018 Intel Corportation
 * Copyright (C) 2013 Red Hat Inc
 * Copyright (c) 2015 HUAWEI TECHNOLOGIES CO.,LTD.
 * Copyright (C) 2008-2010  Kevin O'Connor <kevin@koconnor.net>
 * Copyright (C) 2006 Fabrice Bellard
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu-common.h"

#include "hw/acpi/acpi-defs.h"
#include "hw/acpi/acpi.h"
#include "hw/acpi/aml-build.h"
#include "hw/acpi/bios-linker-loader.h"
#include "hw/acpi/cpu.h"
#include "hw/acpi/pc-hotplug.h"
#include "hw/acpi/reduced.h"
#include "qemu/range.h"
#include "hw/nvram/fw_cfg.h"

#include "hw/pci/pcie_host.h"
#include "hw/pci/pci.h"
#include "hw/i386/virt.h"

#include "hw/loader.h"
#include "hw/hw.h"

#include "sysemu/numa.h"

#include "migration/vmstate.h"

#define GED_DEVICE "GED"

static void acpi_dsdt_add_cpus(MachineState *ms, Aml *dsdt, Aml *scope, int smp_cpus, AcpiConfiguration *conf)
{
    uint16_t i;

    for (i = 0; i < smp_cpus; i++) {
        Aml *dev = aml_device("C%.03X", i);
        aml_append(dev, aml_name_decl("_HID", aml_string("ACPI0007")));
        aml_append(dev, aml_name_decl("_UID", aml_int(i)));
        aml_append(scope, dev);
    }

    CPUHotplugFeatures opts = {
        .apci_1_compatible = false,
        .has_legacy_cphp = false,
    };

    build_cpus_aml(dsdt, ms, opts, conf->cpu_hotplug_io_base,
                   "\\_SB.PCI1", GED_DEVICE);
}

static void acpi_dsdt_add_ged(Aml *scope, AcpiConfiguration *conf)
{
    if (!conf->ged_events || !conf->ged_events_size) {
        return;
    }

    build_ged_aml(scope, GED_DEVICE, conf->ged_events, conf->ged_events_size);
}

/* DSDT */
static void build_dsdt(MachineState *ms, GArray *table_data, BIOSLinker *linker, AcpiPciBus *pci_host, AcpiConfiguration *conf)
{
    Aml *scope, *dsdt;

    dsdt = init_aml_allocator();
    /* Reserve space for header */
    acpi_data_push(dsdt->buf, sizeof(AcpiTableHeader));

    scope = aml_scope("\\_SB");
    acpi_dsdt_add_cpus(ms, dsdt, scope, smp_cpus, conf);
    acpi_dsdt_add_pci_bus(scope, pci_host);
    acpi_dsdt_add_ged(scope, conf);

    aml_append(dsdt, scope);
    /* copy AML table into ACPI tables blob and patch header there */
    g_array_append_vals(table_data, dsdt->buf->data, dsdt->buf->len);
    build_header(linker, table_data,
        (void *)(table_data->data + table_data->len - dsdt->buf->len),
        "DSDT", dsdt->buf->len, 2, NULL, NULL);
    free_aml_allocator();
}


static void build_fadt_reduced(GArray *table_data, BIOSLinker *linker,
                               unsigned dsdt_tbl_offset)
{
    /* ACPI v5.1 */
    AcpiFadtData fadt = {
        .rev = 5,
        .minor_ver = 1,
        .flags = 1 << ACPI_FADT_F_HW_REDUCED_ACPI,
        .dsdt_tbl_offset = &dsdt_tbl_offset,
        .xdsdt_tbl_offset = &dsdt_tbl_offset,
        .arm_boot_arch = 0,
    };

    build_fadt(table_data, linker, &fadt, NULL, NULL);
}

static void acpi_reduced_build(MachineState *ms, AcpiBuildTables *tables, AcpiConfiguration *conf)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    GArray *table_offsets;
    unsigned dsdt, xsdt;
    Range pci_hole, pci_hole64;
    AcpiMcfgInfo mcfg;
    GArray *tables_blob = tables->table_data;

    acpi_get_pci_holes(&pci_hole, &pci_hole64);
    table_offsets = g_array_new(false, true /* clear */,
                                        sizeof(uint32_t));

    bios_linker_loader_alloc(tables->linker,
                             ACPI_BUILD_TABLE_FILE, tables_blob,
                             64, false /* high memory */);

    AcpiPciBus pci_host = {
        .pci_bus    = VIRT_MACHINE(ms)->pci_bus,
        .pci_hole   = &pci_hole,
        .pci_hole64 = &pci_hole64,
    };

    /* DSDT is pointed to by FADT */
    dsdt = tables_blob->len;
    build_dsdt(ms, tables_blob, tables->linker, &pci_host, conf);

    /* FADT pointed to by RSDT */
    acpi_add_table(table_offsets, tables_blob);
    build_fadt_reduced(tables_blob, tables->linker, dsdt);

    /* MADT pointed to by RSDT */
    acpi_add_table(table_offsets, tables_blob);
    mc->firmware_build_methods.acpi.madt(tables_blob, tables->linker, ms, conf);

    if (acpi_get_mcfg(&mcfg)) {
        acpi_add_table(table_offsets, tables_blob);
        mc->firmware_build_methods.acpi.mcfg(tables_blob, tables->linker, &mcfg);
    }
    /* RSDT is pointed to by RSDP */
    xsdt = tables_blob->len;
    build_xsdt(tables_blob, tables->linker, table_offsets, NULL, NULL);

    /* RSDP is in FSEG memory, so allocate it separately */
    mc->firmware_build_methods.acpi.rsdp(tables->rsdp, tables->linker, xsdt);
    acpi_align_size(tables->linker->cmd_blob, ACPI_BUILD_ALIGN_SIZE);

    /* Cleanup memory that's no longer used. */
    g_array_free(table_offsets, true);
}

static void acpi_ram_update(MemoryRegion *mr, GArray *data)
{
    uint32_t size = acpi_data_len(data);

    /* Make sure RAM size is correct - in case it got changed
     * e.g. by migration */
    memory_region_ram_resize(mr, size, &error_abort);

    memcpy(memory_region_get_ram_ptr(mr), data->data, size);
    memory_region_set_dirty(mr, 0, size);
}

static void acpi_reduced_build_update(void *build_opaque)
{
    MachineState *ms = MACHINE(build_opaque);
    AcpiBuildState *build_state = ms->firmware_build_state.acpi.state;
    AcpiConfiguration *conf = ms->firmware_build_state.acpi.conf;
    AcpiBuildTables tables;

    /* No ACPI configuration? Nothing to do. */
    if (!conf) {
        return;
    }

    /* No state to update or already patched? Nothing to do. */
    if (!build_state || build_state->patched) {
        return;
    }
    build_state->patched = true;

    acpi_build_tables_init(&tables);

    acpi_reduced_build(ms, &tables, conf);

    acpi_ram_update(build_state->table_mr, tables.table_data);
    acpi_ram_update(build_state->rsdp_mr, tables.rsdp);
    acpi_ram_update(build_state->linker_mr, tables.linker->cmd_blob);

    acpi_build_tables_cleanup(&tables, true);
}

static void acpi_reduced_build_reset(void *build_opaque)
{
    MachineState *ms = MACHINE(build_opaque);
    AcpiBuildState *build_state = ms->firmware_build_state.acpi.state;

    build_state->patched = false;
}

static MemoryRegion *acpi_add_rom_blob(MachineState *ms,
                                       GArray *blob, const char *name,
                                       uint64_t max_size)
{
    return rom_add_blob(name, blob->data, acpi_data_len(blob), max_size, -1,
                        name, acpi_reduced_build_update, ms, NULL, true);
}

static const VMStateDescription vmstate_acpi_reduced_build = {
    .name = "acpi_reduced_build",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_BOOL(patched, AcpiBuildState),
        VMSTATE_END_OF_LIST()
    },
};

void acpi_reduced_setup(MachineState *machine, AcpiConfiguration *conf)
{
    AcpiBuildTables tables;
    AcpiBuildState *build_state;

    build_state = g_malloc0(sizeof(*build_state));
    machine->firmware_build_state.acpi.state = build_state;
    machine->firmware_build_state.acpi.conf = conf;

    acpi_build_tables_init(&tables);
    acpi_reduced_build(machine, &tables, conf);

    if (conf->fw_cfg) {
        /* Now expose it all to Guest */
        build_state->table_mr = acpi_add_rom_blob(machine, tables.table_data,
                                                  ACPI_BUILD_TABLE_FILE,
                                                  ACPI_BUILD_TABLE_MAX_SIZE);
        assert(build_state->table_mr != NULL);

        build_state->linker_mr =
            acpi_add_rom_blob(machine, tables.linker->cmd_blob,
                              "etc/table-loader", 0);

        fw_cfg_add_file(conf->fw_cfg, ACPI_BUILD_TPMLOG_FILE, tables.tcpalog->data,
                        acpi_data_len(tables.tcpalog));

        build_state->rsdp_mr = acpi_add_rom_blob(machine, tables.rsdp,
                                                 ACPI_BUILD_RSDP_FILE, 0);
    }

    qemu_register_reset(acpi_reduced_build_reset, machine);
    acpi_reduced_build_reset(machine);
    vmstate_register(NULL, 0, &vmstate_acpi_reduced_build, machine);

    if (!conf->fw_cfg) {
        acpi_link(conf, tables.linker, &error_abort);
        build_state->patched = 1;
    }

    /* Cleanup tables but don't free the memory: we track it
     * in build_state.
     */
    acpi_build_tables_cleanup(&tables, false);
}

#define CPU_SCAN_METHOD   "CSCN"

static Aml *ged_event_aml(GedEvent *event)
{
    if (!event) {
        return NULL;
    }

    switch (event->event) {
    case GED_CPU_HOTPLUG:
        /* We run a complete CPU SCAN when getting a CPU hotplug event */
        return aml_call0("\\_SB.CPUS." CPU_SCAN_METHOD);
    case GED_MEMORY_HOTPLUG:
    case GED_PCI_HOTPLUG:
    case GED_NVDIMM_HOTPLUG:
        /* Not supported for now */
        return NULL;
    default:
        break;
    }

    return NULL;
}

void build_ged_aml(Aml *table, const char *name,
                   GedEvent *events, uint8_t events_size)
{
    Aml *crs = aml_resource_template();
    Aml *evt;
    Aml *zero = aml_int(0);
    Aml *one = aml_int(1);
    Aml *dev = aml_device("%s", name);
    Aml *scope = aml_scope("_SB");
    Aml *has_irq = aml_local(0);
    Aml *while_ctx;
    uint8_t i;

    /*
     * For each GED event we:
     * - Add an interrupt to the CRS section.
     * - Add a conditional block for each event, inside a while loop.
     *   This is semantically equivalent to a switch/case implementation.
     */
    evt = aml_method("_EVT", 1, AML_SERIALIZED);
    {
        Aml *irq = aml_arg(0);
        Aml *ged_aml;
        Aml *if_ctx, *else_ctx;

        /* Local0 = One */
        aml_append(evt, aml_store(one, has_irq));


        /*
         * Here we want to call a method for each supported GED event type.
         * The resulting ASL code looks like:
         *
         * Local0 = One
         * While ((Local0 == One))
         * {
         *    Local0 = Zero
         *    If (Arg0 == irq0)
         *    {
         *        MethodEvent0()
         *        Local0 = Zero
         *    }
         *    ElseIf (Arg0 == irq1)
         *    {
         *        MethodEvent1()
         *        Local0 = Zero
         *    }
         *    ElseIf (Arg0 == irq2)
         *    {
         *        MethodEvent2()
         *        Local0 = Zero
         *    }
         * }
         */

        /* While ((Local0 == One)) */
        while_ctx = aml_while(aml_equal(has_irq, one));
        {
            else_ctx = NULL;

            /*
             * Clear loop condition, we don't want to enter an infinite loop.
             * Local0 = Zero
             */
            aml_append(while_ctx, aml_store(zero, has_irq));
            for (i = 0; i < events_size; i++) {
                ged_aml = ged_event_aml(&events[i]);
                if (!ged_aml) {
                    continue;
                }

                /* _CRS interrupt */
                aml_append(crs, aml_interrupt(AML_CONSUMER, AML_LEVEL, AML_ACTIVE_HIGH,
                                              AML_EXCLUSIVE, &events[i].irq, 1));

                /* If ((Arg0 == irq))*/
                if_ctx = aml_if(aml_equal(irq, aml_int(events[i].irq)));
                {
                    /* AML for this specific type of event */
                    aml_append(if_ctx, ged_aml);
                }

                /*
                 * We append the first if to the while context.
                 * Other ifs will be elseifs.
                 */
                if (!else_ctx) {
                    aml_append(while_ctx, if_ctx);
                } else {
                    aml_append(else_ctx, if_ctx);
                    aml_append(while_ctx, else_ctx);
                }

                if (i != events_size - 1) {
                    else_ctx = aml_else();
                }
            }
        }

        aml_append(evt, while_ctx);
    }

    aml_append(dev, aml_name_decl("_HID", aml_string("ACPI0013")));
    aml_append(dev, aml_name_decl("_UID", zero));
    aml_append(dev, aml_name_decl("_CRS", crs));
    aml_append(dev, evt);
    aml_append(scope, dev);

    aml_append(table, scope);
}
