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

#ifndef QEMU_I386_VIRT_H
#define QEMU_I386_VIRT_H

#include "qemu-common.h"
#include "exec/hwaddr.h"
#include "qemu/notify.h"

#include "hw/boards.h"
#include "hw/acpi/acpi.h"

typedef struct {
    MachineClass parent;
    HotplugHandler *(*orig_hotplug_handler)(MachineState *machine,
                                           DeviceState *dev);
} VirtMachineClass;

typedef struct {
    MachineState parent;
    unsigned apic_id_limit;

    Notifier machine_done;

    /* ACPI configuration */
    AcpiConfiguration acpi_conf;

    /* number of CPUs */
    uint16_t boot_cpus;

    /* GSI */
    qemu_irq *gsi;

    PCIBus *pci_bus;
    ram_addr_t above_4g_mem_size;

    DeviceState *acpi;
} VirtMachineState;

#define VIRT_MACHINE_NVDIMM "nvdimm"

#define TYPE_VIRT_MACHINE   MACHINE_TYPE_NAME("virt")
#define VIRT_MACHINE(obj) \
    OBJECT_CHECK(VirtMachineState, (obj), TYPE_VIRT_MACHINE)
#define VIRT_MACHINE_GET_CLASS(obj) \
    OBJECT_GET_CLASS(VirtMachineClass, obj, TYPE_VIRT_MACHINE)
#define VIRT_MACHINE_CLASS(class) \
    OBJECT_CLASS_CHECK(VirtMachineClass, class, TYPE_VIRT_MACHINE)

/* Our first GED IRQ lives after the legacy IRQs */
#define VIRT_GED_IRQ_BASE           16
#define VIRT_GED_CPU_HOTPLUG_IRQ    VIRT_GED_IRQ_BASE
#define VIRT_GED_MEMORY_HOTPLUG_IRQ VIRT_GED_IRQ_BASE + 1
#define VIRT_GED_PCI_HOTPLUG_IRQ    VIRT_GED_IRQ_BASE + 2
#define VIRT_GED_NVDIMM_HOTPLUG_IRQ VIRT_GED_IRQ_BASE + 3

MemoryRegion *virt_memory_init(VirtMachineState *vms);

DeviceState *virt_acpi_init(qemu_irq *gsi, PCIBus *pci_bus);

#endif
