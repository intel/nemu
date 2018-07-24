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
#include "hw/acpi/reduced.h"

#include "hw/nvram/fw_cfg.h"

#include "hw/pci/pcie_host.h"
#include "hw/pci/pci.h"

#include "hw/loader.h"
#include "hw/hw.h"

#include "sysemu/numa.h"

#include "migration/vmstate.h"

static void acpi_dsdt_add_cpus(Aml *scope, int smp_cpus)
{
    uint16_t i;

    for (i = 0; i < smp_cpus; i++) {
        Aml *dev = aml_device("C%.03X", i);
        aml_append(dev, aml_name_decl("_HID", aml_string("ACPI0007")));
        aml_append(dev, aml_name_decl("_UID", aml_int(i)));
        aml_append(scope, dev);
    }
}

/* DSDT */
static void build_dsdt(GArray *table_data, BIOSLinker *linker)
{
    Aml *scope, *dsdt, *dev;

    dsdt = init_aml_allocator();
    /* Reserve space for header */
    acpi_data_push(dsdt->buf, sizeof(AcpiTableHeader));

    /* When booting the VM with UEFI, UEFI takes ownership of the RTC hardware.
     * While UEFI can use libfdt to disable the RTC device node in the DTB that
     * it passes to the OS, it cannot modify AML. Therefore, we won't generate
     * the RTC ACPI device at all when using UEFI.
     */
    scope = aml_scope("\\_SB");
    acpi_dsdt_add_cpus(scope, smp_cpus);

    dev = aml_device("PCI0");
    aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0A08")));
    aml_append(dev, aml_name_decl("_CID", aml_eisaid("PNP0A03")));
    aml_append(dev, aml_name_decl("_ADR", aml_int(0)));
    aml_append(dev, aml_name_decl("_UID", aml_int(1)));
    aml_append(scope, dev);
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
    GArray *tables_blob = tables->table_data;

    table_offsets = g_array_new(false, true /* clear */,
                                        sizeof(uint32_t));

    bios_linker_loader_alloc(tables->linker,
                             ACPI_BUILD_TABLE_FILE, tables_blob,
                             64, false /* high memory */);

    /* DSDT is pointed to by FADT */
    dsdt = tables_blob->len;
    build_dsdt(tables_blob, tables->linker);

    /* FADT pointed to by RSDT */
    acpi_add_table(table_offsets, tables_blob);
    build_fadt_reduced(tables_blob, tables->linker, dsdt);

    /* MADT pointed to by RSDT */
    acpi_add_table(table_offsets, tables_blob);
    mc->firmware_build_methods.acpi.madt(tables_blob, tables->linker, ms, conf);

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
