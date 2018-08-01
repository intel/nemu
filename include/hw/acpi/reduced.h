/* HW reduced ACPI headers
 *
 * Copyright (c) 2018 Intel Corportation
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

#ifndef HW_ACPI_REDUCED_H
#define HW_ACPI_REDUCED_H

#define ACPI_REDUCED_SLEEP_LEVEL          5
#define ACPI_REDUCED_SLEEP_ENABLE         (1 << 5) /* SLP_EN */
#define ACPI_REDUCED_SLEEP_CONTROL_IOPORT 0x3B0
#define ACPI_REDUCED_RESET_IOPORT         0x3C0
#define ACPI_REDUCED_RESET_VALUE          4

typedef struct Aml Aml;

typedef enum {
    GED_CPU_HOTPLUG    = 1,
    GED_MEMORY_HOTPLUG = 2,
    GED_PCI_HOTPLUG    = 3,
    GED_NVDIMM_HOTPLUG = 4,
} GedEventType;

typedef struct GedEvent {
    uint32_t     irq;
    GedEventType event;
} GedEvent;

void acpi_reduced_setup(MachineState *machine, AcpiConfiguration *conf);
void build_ged_aml(Aml *table, const char* name,
                   GedEvent *events, uint8_t events_size);

#endif
