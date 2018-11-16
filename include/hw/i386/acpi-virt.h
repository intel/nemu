/*
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
#ifndef QEMU_ACPI_VIRT_H
#define QEMU_ACPI_VIRT_H

#include "hw/acpi/cpu_hotplug.h"
#include "hw/acpi/memory_hotplug.h"
#include "hw/acpi/pc-hotplug.h"
#include "hw/acpi/pcihp.h"


typedef struct VirtAcpiState {
    SysBusDevice parent_obj;

    AcpiCpuHotplug cpuhp;
    CPUHotplugState cpuhp_state;

    MemHotplugState memhp_state;

    GEDState ged_state;

    qemu_irq *gsi;

    AcpiPciHpState **pcihp_state;
    PCIBus *pci_bus;

    MemoryRegion sleep_iomem;
    MemoryRegion sleep_status_iomem;
    MemoryRegion reset_iomem;
    MemoryRegion pm_timer_iomem;
} VirtAcpiState;

#define TYPE_VIRT_ACPI "virt-acpi"
#define VIRT_ACPI(obj) \
    OBJECT_CHECK(VirtAcpiState, (obj), TYPE_VIRT_ACPI)

#endif
