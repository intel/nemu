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

#ifndef HW_ACPI_GED_H
#define HW_ACPI_GED_H

#include "qemu/osdep.h"
#include "exec/memory.h"
#include "hw/acpi/aml-build.h"
#include "hw/acpi/cpu.h"
#include "hw/acpi/memory_hotplug.h"

#define GED_DEVICE "GED"

typedef enum {
    GED_CPU_HOTPLUG    = 1,
    GED_MEMORY_HOTPLUG = 2,
    GED_NVDIMM_HOTPLUG = 3,
    GED_PCI_HOTPLUG    = 4,
} GedEventType;

typedef struct GedEvent {
    uint32_t     irq;
    GedEventType event;
} GedEvent;

void build_ged_aml(Aml *table, const char* name,
                   GedEvent *events, uint8_t events_size);

#endif
