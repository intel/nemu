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

#ifndef ACPI_BUILDER_H
#define ACPI_BUILDER_H

#include "qemu/osdep.h"
#include "hw/acpi/bios-linker-loader.h"
#include "qom/object.h"

#define TYPE_ACPI_BUILDER "acpi-builder"

#define ACPI_BUILDER_METHODS(klass) \
     OBJECT_CLASS_CHECK(AcpiBuilderMethods, (klass), TYPE_ACPI_BUILDER)
#define ACPI_BUILDER_GET_METHODS(obj) \
     OBJECT_GET_CLASS(AcpiBuilderMethods, (obj), TYPE_ACPI_BUILDER)
#define ACPI_BUILDER(obj)                                       \
     INTERFACE_CHECK(AcpiBuilder, (obj), TYPE_ACPI_BUILDER)

typedef struct AcpiConfiguration AcpiConfiguration;
typedef struct AcpiBuildState AcpiBuildState;
typedef struct AcpiMcfgInfo AcpiMcfgInfo;

typedef struct AcpiBuilder {
    /* <private> */
    Object Parent;
} AcpiBuilder;

/**
 * AcpiBuildMethods:
 *
 * Interface to be implemented by a machine type that needs to provide
 * custom ACPI tables build method.
 *
 * @parent: Opaque parent interface.
 *
 * @rsdp:
 * @madt:
 * @mcfg:
 * @srat:
 * @slit:
 * @setup:
 */
typedef struct AcpiBuilderMethods {
    /* <private> */
    InterfaceClass parent;

    /* <public> */
    void (*rsdp)(GArray *table_data, BIOSLinker *linker,
                 unsigned rsdt_tbl_offset);
    void (*madt)(GArray *table_data, BIOSLinker *linker,
                 MachineState *ms, AcpiConfiguration *conf);
    void (*mcfg)(GArray *table_data, BIOSLinker *linker,
                 AcpiMcfgInfo *info);
    void (*srat)(GArray *table_data, BIOSLinker *linker,
                 MachineState *machine, AcpiConfiguration *conf);
    void (*slit)(GArray *table_data, BIOSLinker *linker);

    AcpiConfiguration *(*configuration)(AcpiBuilder *builder);
} AcpiBuilderMethods;

void acpi_builder_rsdp(AcpiBuilder *builder,
                       GArray *table_data, BIOSLinker *linker,
                       unsigned rsdt_tbl_offset);

void acpi_builder_madt(AcpiBuilder *builder,
                       GArray *table_data, BIOSLinker *linker,
                       MachineState *ms, AcpiConfiguration *conf);

void acpi_builder_mcfg(AcpiBuilder *builder,
                       GArray *table_data, BIOSLinker *linker,
                       AcpiMcfgInfo *info);

void acpi_builder_srat(AcpiBuilder *builder,
                       GArray *table_data, BIOSLinker *linker,
                       MachineState *machine, AcpiConfiguration *conf);

void acpi_builder_slit(AcpiBuilder *builder,
                       GArray *table_data, BIOSLinker *linker);

AcpiConfiguration *acpi_builder_configuration(AcpiBuilder *builder);

#endif
