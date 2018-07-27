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

#ifndef FW_BUILD_H
#define FW_BUILD_H

#include "hw/acpi/bios-linker-loader.h"

typedef struct AcpiConfiguration AcpiConfiguration;
typedef struct AcpiBuildState AcpiBuildState;
typedef struct AcpiMcfgInfo AcpiMcfgInfo;

typedef struct FirmwareBuildMethods {
    union {
        /* ACPI methods */
        struct {
            GArray *(*rsdp)(GArray *table_data, BIOSLinker *linker, unsigned rsdt_tbl_offset);
            GArray *(*madt)(GArray *table_data, BIOSLinker *linker, MachineState *ms, AcpiConfiguration *conf);
            void    (*setup)(MachineState *ms, AcpiConfiguration *conf);
            void    (*mcfg)(GArray *table_data, BIOSLinker *linker, AcpiMcfgInfo *info);
        } acpi;
    };
} FirmwareBuildMethods;

typedef struct FirmwareBuildState {
    union {
        /* ACPI state and configuration */
        struct {
            AcpiConfiguration *conf;
            AcpiBuildState *state;
        } acpi;
    };
} FirmwareBuildState;

#endif
