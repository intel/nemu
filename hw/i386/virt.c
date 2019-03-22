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
#include "qapi/error.h"

#include "hw/i386/virt.h"
#include "hw/i386/amd_iommu.h"
#include "hw/i386/intel_iommu.h"

#define DEFINE_VIRT_MACHINE_LATEST(major, minor, latest) \
    static void virt_##major##_##minor##_object_class_init(ObjectClass *oc, \
                                                           void *data)  \
    { \
        MachineClass *mc = MACHINE_CLASS(oc); \
        virt_##major##_##minor##_machine_class_init(mc); \
        mc->desc = "QEMU " # major "." # minor " i386 Virtual Machine"; \
        if (latest) { \
            mc->alias = "virt"; \
        } \
    } \
    static const TypeInfo virt_##major##_##minor##_info = { \
        .name = MACHINE_TYPE_NAME("virt-" # major "." # minor), \
        .parent = TYPE_VIRT_MACHINE, \
        .instance_init = virt_##major##_##minor##_instance_init, \
        .class_init = virt_##major##_##minor##_object_class_init, \
    }; \
    static void virt_##major##_##minor##_init(void) \
    { \
        type_register_static(&virt_##major##_##minor##_info); \
    } \
    type_init(virt_##major##_##minor##_init);

#define DEFINE_VIRT_MACHINE_AS_LATEST(major, minor) \
    DEFINE_VIRT_MACHINE_LATEST(major, minor, true)
#define DEFINE_VIRT_MACHINE(major, minor) \
    DEFINE_VIRT_MACHINE_LATEST(major, minor, false)

static void virt_class_init(ObjectClass *oc, void *data)
{
}

static const TypeInfo virt_machine_info = {
    .name          = TYPE_VIRT_MACHINE,
    .parent        = TYPE_MACHINE,
    .abstract      = true,
    .instance_size = sizeof(VirtMachineState),
    .class_size    = sizeof(VirtMachineClass),
    .class_init    = virt_class_init,
};

static void virt_machine_init(void)
{
    type_register_static(&virt_machine_info);
}
type_init(virt_machine_init);

static void virt_2_12_instance_init(Object *obj)
{
}

static void virt_machine_class_init(MachineClass *mc)
{
    mc->family = "virt_i386";
    mc->desc = "Virtual i386 machine";
    mc->units_per_default_bus = 1;
    mc->no_floppy = 1;
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_AMD_IOMMU_DEVICE);
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_INTEL_IOMMU_DEVICE);
    mc->max_cpus = 288;
}

static void virt_2_12_machine_class_init(MachineClass *mc)
{
    virt_machine_class_init(mc);
    mc->alias = "virt";
}
DEFINE_VIRT_MACHINE_AS_LATEST(2, 12)
