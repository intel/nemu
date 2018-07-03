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
#include "hw/i386/acpi.h"

typedef struct {
    MachineClass parent;
} VirtMachineClass;

typedef struct {
    MachineState parent;
    unsigned apic_id_limit;

    Notifier machine_done;

    /* NUMA information: */
    uint64_t numa_nodes;
    uint64_t *node_mem;

    /* ACPI configuration */
    AcpiConfiguration *acpi_configuration;

    /* Devices and objects */
    FWCfgState *fw_cfg;
} VirtMachineState;

#define TYPE_VIRT_MACHINE   MACHINE_TYPE_NAME("virt")
#define VIRT_MACHINE(obj) \
    OBJECT_CHECK(VirtMachineState, (obj), TYPE_VIRT_MACHINE)
#define VIRT_MACHINE_GET_CLASS(obj) \
    OBJECT_GET_CLASS(VirtMachineClass, obj, TYPE_VIRT_MACHINE)
#define VIRT_MACHINE_CLASS(class) \
    OBJECT_CLASS_CHECK(VirtMachineClass, class, TYPE_VIRT_MACHINE)

MemoryRegion *virt_memory_init(VirtMachineState *vms);

#endif
