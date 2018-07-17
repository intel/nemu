/*
 *
 * Copyright (c) 2018 Intel Corporation
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
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/acpi/builder.h"

void acpi_builder_rsdp(AcpiBuilder *builder,
                       GArray *table_data, BIOSLinker *linker,
                       unsigned rsdt_tbl_offset)
{
    AcpiBuilderMethods *abm = ACPI_BUILDER_GET_METHODS(builder);

    if (abm && abm->rsdp) {
        abm->rsdp(table_data, linker, rsdt_tbl_offset);
    }
}

void acpi_builder_madt(AcpiBuilder *builder,
                       GArray *table_data, BIOSLinker *linker,
                       MachineState *ms, AcpiConfiguration *conf)
{
    AcpiBuilderMethods *abm = ACPI_BUILDER_GET_METHODS(builder);

    if (abm && abm->madt) {
        abm->madt(table_data, linker, ms, conf);
    }
}

void acpi_builder_mcfg(AcpiBuilder *builder,
                       GArray *table_data, BIOSLinker *linker,
                       AcpiMcfgInfo *info)
{
    AcpiBuilderMethods *abm = ACPI_BUILDER_GET_METHODS(builder);

    if (abm && abm->mcfg) {
        abm->mcfg(table_data, linker, info);
    }
}

void acpi_builder_srat(AcpiBuilder *builder,
                       GArray *table_data, BIOSLinker *linker,
                       MachineState *machine, AcpiConfiguration *conf)
{
    AcpiBuilderMethods *abm = ACPI_BUILDER_GET_METHODS(builder);

    if (abm && abm->srat) {
        abm->srat(table_data, linker, machine, conf);
    }
}

void acpi_builder_slit(AcpiBuilder *builder,
                       GArray *table_data, BIOSLinker *linker)
{
    AcpiBuilderMethods *abm = ACPI_BUILDER_GET_METHODS(builder);

    if (abm && abm->slit) {
        abm->slit(table_data, linker);
    }
}

AcpiConfiguration *acpi_builder_configuration(AcpiBuilder *builder)
{
    AcpiBuilderMethods *abm = ACPI_BUILDER_GET_METHODS(builder);
    if (abm && abm->configuration) {
        return abm->configuration(builder);
    }
    return NULL;
}

static const TypeInfo acpi_builder_info = {
    .name          = TYPE_ACPI_BUILDER,
    .parent        = TYPE_INTERFACE,
    .class_size    = sizeof(AcpiBuilderMethods),
};

static void acpi_builder_register_type(void)
{
    type_register_static(&acpi_builder_info);
}

type_init(acpi_builder_register_type)
