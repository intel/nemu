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
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "sysemu/sysemu.h"
#include "sysemu/cpus.h"
#include "sysemu/numa.h"

#include "hw/loader.h"
#include "hw/nmi.h"

#include "hw/kvm/clock.h"

#include "hw/i386/virt.h"
#include "hw/i386/acpi.h"
#include "hw/i386/cpu-internal.h"
#include "hw/i386/fw.h"
#include "hw/i386/kernel-loader.h"
#include "hw/i386/topology.h"
#include "hw/i386/amd_iommu.h"
#include "hw/i386/intel_iommu.h"

#include "cpu.h"
#include "kvm_i386.h"

#include "../acpi-build.h"

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

static void acpi_conf_virt_init(MachineState *machine, AcpiConfiguration *conf)
{
    VirtMachineState *vms = VIRT_MACHINE(machine);

    if (!conf) {
        error_report("The ACPI configuration structure must be allocated");
        exit(EXIT_FAILURE);
    }

    conf->legacy_acpi_table_size = 0;
    conf->legacy_cpu_hotplug = false;
    conf->rsdp_in_ram = true;
    conf->apic_xrupt_override = kvm_allows_irq0_override();

    /* TODO: lowmem, acpi_dev, acpi_nvdimm, hotplug_memory */
    conf->fw_cfg = vms->fw_cfg;
    conf->numa_nodes = vms->numa_nodes;
    conf->node_mem = vms->node_mem;
    conf->apic_id_limit = vms->apic_id_limit;
}

static void virt_machine_done(Notifier *notifier, void *data)
{
    VirtMachineState *vms = container_of(notifier,
                                          VirtMachineState, machine_done);
    AcpiConfiguration *conf;

    conf = g_malloc0(sizeof(*conf));
    vms->acpi_configuration = conf;

    acpi_conf_virt_init(MACHINE(vms), conf);
    acpi_setup(MACHINE(vms), conf);
}

static void virt_machine_state_init(MachineState *machine)
{
    int i;
    FWCfgState *fw_cfg;
    //MemoryRegion *ram;
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    VirtMachineState *vms = VIRT_MACHINE(machine);
    bool linux_boot = (machine->kernel_filename != NULL);

    /* NUMA stuff */
    vms->numa_nodes = nb_numa_nodes;
    vms->node_mem = g_malloc0(nb_numa_nodes *
                              sizeof *vms->node_mem);
    for (i = 0; i < nb_numa_nodes; i++) {
        vms->node_mem[i] = numa_info[i].node_mem;
    }

    vms->machine_done.notify = virt_machine_done;
    qemu_add_machine_init_done_notifier(&vms->machine_done);

    /* TODO Add the ram pointer to the QOM */
    virt_memory_init(vms);

    vms->apic_id_limit = cpus_init(machine, false);

    kvmclock_create();

    if (vms->fw) {
        fw_cfg = fw_cfg_init(machine, smp_cpus, mc->possible_cpu_arch_ids(machine), vms->apic_id_limit);
        rom_set_fw(fw_cfg);
        vms->fw_cfg = fw_cfg;
        if (linux_boot) {
            load_linux_bzimage(MACHINE(vms), vms->acpi_configuration, fw_cfg);
        }
    } else {
        if (linux_boot) {
            load_linux_efi(MACHINE(vms));
        }
    }
}

static bool virt_machine_get_fw(Object *obj, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    return vms->fw;
}

static void virt_machine_set_fw(Object *obj, bool value, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    vms->fw = value;
}

static void virt_machine_instance_init(Object *obj)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    vms->fw = true;
}

static void virt_machine_reset(void)
{
    CPUState *cs;
    X86CPU *cpu;

    qemu_devices_reset();

    CPU_FOREACH(cs) {
        cpu = X86_CPU(cs);

        /* Reset APIC after devices have been reset to cancel
         * any changes that qemu_devices_reset() might have done.
         */
        if (cpu->apic_state) {
            device_reset(cpu->apic_state);
        }

        if (boot_info.protected_mode && cpu_is_bsp(cpu)) {
            kernel_loader_reset_cpu(&cpu->env);
        }
    }

    if (boot_info.protected_mode) {
        kernel_loader_setup();
    }
}

static void x86_nmi(NMIState *n, int cpu_index, Error **errp)
{
    CPUState *cs;

    CPU_FOREACH(cs) {
        X86CPU *cpu = X86_CPU(cs);

        if (!cpu->apic_state) {
            cpu_interrupt(cs, CPU_INTERRUPT_NMI);
        } else {
            apic_deliver_nmi(cpu->apic_state);
        }
    }
}

static void virt_class_init(ObjectClass *oc, void *data)
{
    NMIClass *nc = NMI_CLASS(oc);

    /* NMI handler */
    nc->nmi_monitor_handler = x86_nmi;

    /* Firmware setting */
    object_class_property_add_bool(oc, VIRT_MACHINE_FW,
                                   virt_machine_get_fw, virt_machine_set_fw, &error_abort);

}

static const TypeInfo virt_machine_info = {
    .name          = TYPE_VIRT_MACHINE,
    .parent        = TYPE_MACHINE,
    .abstract      = true,
    .instance_size = sizeof(VirtMachineState),
    .instance_init = virt_machine_instance_init,
    .class_size    = sizeof(VirtMachineClass),
    .class_init    = virt_class_init,
    .interfaces = (InterfaceInfo[]) {
         { TYPE_NMI },
         { }
    },
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
    mc->init = virt_machine_state_init;

    mc->family = "virt_i386";
    mc->desc = "Virtual i386 machine";
    mc->units_per_default_bus = 1;
    mc->no_floppy = 1;
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_AMD_IOMMU_DEVICE);
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_INTEL_IOMMU_DEVICE);
    mc->max_cpus = 288;
    mc->has_hotpluggable_cpus = true;

    /* Machine class handlers */
    mc->cpu_index_to_instance_props = cpu_index_to_props;
    mc->get_default_cpu_node_id = cpu_get_default_cpu_node_id;
    mc->possible_cpu_arch_ids = cpu_possible_cpu_arch_ids;;
    mc->reset = virt_machine_reset;
}

static void virt_2_12_machine_class_init(MachineClass *mc)
{
    virt_machine_class_init(mc);
    mc->alias = "virt";
}
DEFINE_VIRT_MACHINE_AS_LATEST(2, 12)
