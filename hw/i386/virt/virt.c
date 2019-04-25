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

#include "hw/acpi/acpi.h"
#include "hw/acpi/reduced.h"
#include "hw/acpi/aml-build.h"

#include "hw/i386/virt.h"
#include "hw/i386/cpu-internal.h"
#include "hw/i386/fw.h"
#include "hw/i386/kernel-loader.h"
#include "hw/i386/topology.h"
#include "hw/i386/amd_iommu.h"
#include "hw/i386/intel_iommu.h"

#include "hw/pci-host/pci-lite.h"

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

static void acpi_conf_virt_init(MachineState *machine)
{
    VirtMachineState *vms = VIRT_MACHINE(machine);
    AcpiConfiguration *conf;

    if (!vms->acpi_configuration) {
        vms->acpi_configuration = g_malloc0(sizeof(AcpiConfiguration));
    }

    conf = vms->acpi_configuration;
    conf->legacy_acpi_table_size = 0;
    conf->legacy_cpu_hotplug = false;
    conf->rsdp_in_ram = true;
    conf->apic_xrupt_override = kvm_allows_irq0_override();

    /* TODO: lowmem, acpi_dev, acpi_nvdimm, hotplug_memory */
    conf->fw_cfg = vms->fw_cfg;
    conf->numa_nodes = vms->numa_nodes;
    conf->node_mem = vms->node_mem;
    conf->apic_id_limit = vms->apic_id_limit;
    conf->acpi_dev = vms->acpi_dev;
}

static void virt_machine_done(Notifier *notifier, void *data)
{
    VirtMachineState *vms = container_of(notifier,
                                          VirtMachineState, machine_done);
    MachineState *ms = MACHINE(vms);
    MachineClass *mc = MACHINE_GET_CLASS(ms);

    mc->firmware_build_methods.acpi.setup(ms, vms->acpi_configuration);
}

static void virt_gsi_handler(void *opaque, int n, int level)
{
    qemu_irq *ioapic_irq = opaque;

    qemu_set_irq(ioapic_irq[n], level);
}

static void virt_pci_init(VirtMachineState *vms)
{
    MemoryRegion *pci_memory;
    qemu_irq *ioapic_irq;
    DeviceState *ioapic_dev;
    SysBusDevice *d;
    unsigned int i;

    pci_memory = g_new(MemoryRegion, 1);

    /* irq lines */
    ioapic_irq = g_new0(qemu_irq, IOAPIC_NUM_PINS);
    assert(kvm_irqchip_in_kernel());
    kvm_pc_setup_irq_routing(true);

    qemu_allocate_irqs(virt_gsi_handler, ioapic_irq, IOAPIC_NUM_PINS);

    memory_region_init(pci_memory, NULL, "pci", UINT64_MAX);

    vms->pci_bus = pci_lite_init(get_system_memory(), get_system_io(),
                                 pci_memory);

    assert(kvm_ioapic_in_kernel());
    ioapic_dev = qdev_create(NULL, "kvm-ioapic");

    object_property_add_child(qdev_get_machine(), "ioapic", OBJECT(ioapic_dev), NULL);

    qdev_init_nofail(ioapic_dev);
    d = SYS_BUS_DEVICE(ioapic_dev);
    sysbus_mmio_map(d, 0, IO_APIC_DEFAULT_ADDRESS);

    for (i = 0; i < IOAPIC_NUM_PINS; i++) {
        ioapic_irq[i] = qdev_get_gpio_in(ioapic_dev, i);
    }
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
    vms->acpi = virt_acpi_init();
    virt_memory_init(vms);
    virt_pci_init(vms);

    vms->apic_id_limit = cpus_init(machine, false);

    kvmclock_create();

    object_property_add_link(OBJECT(machine), "acpi-device",
                             TYPE_HOTPLUG_HANDLER,
                             (Object **)&vms->acpi_dev,
                             object_property_allow_set_link,
                             OBJ_PROP_LINK_STRONG, &error_abort);
    object_property_set_link(OBJECT(machine), OBJECT(vms->acpi),
                             "acpi-device", &error_abort);
            
    fw_cfg = fw_cfg_init(machine, smp_cpus, mc->possible_cpu_arch_ids(machine), vms->apic_id_limit);
    rom_set_fw(fw_cfg);
    vms->fw_cfg = fw_cfg;
    acpi_conf_virt_init(machine);

    if (linux_boot) {
        load_linux(MACHINE(vms), vms->acpi_configuration, fw_cfg);
    }
}

static void virt_machine_instance_init(Object *obj)
{
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
         { TYPE_HOTPLUG_HANDLER },
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

static void virt_cpu_plug(HotplugHandler *hotplug_dev,
                        DeviceState *dev, Error **errp)
{
    CPUArchId *found_cpu;
    Error *local_err = NULL;
    X86CPU *cpu = X86_CPU(dev);
    MachineState *ms = MACHINE(hotplug_dev);
    VirtMachineState *vms = VIRT_MACHINE(hotplug_dev);

    // TODO: Toggle ACPI CPU hotplug here

    /* increment the number of CPUs */
    vms->boot_cpus++;

    // TODO: If using RTC update here

    if (vms->fw_cfg) {
        fw_cfg_modify_i16(vms->fw_cfg, FW_CFG_NB_CPUS, vms->boot_cpus);
    }

    found_cpu = cpu_find_slot(ms, cpu->apic_id, NULL);
    found_cpu->cpu = OBJECT(dev);

    error_propagate(errp, local_err);
}

static void virt_cpu_pre_plug(HotplugHandler *hotplug_dev,
                            DeviceState *dev, Error **errp)
{
    int idx;
    CPUState *cs;
    CPUArchId *cpu_slot;
    X86CPUTopoInfo topo;
    X86CPU *cpu = X86_CPU(dev);
    MachineState *ms = MACHINE(hotplug_dev);

    if(!object_dynamic_cast(OBJECT(cpu), ms->cpu_type)) {
        error_setg(errp, "Invalid CPU type, expected cpu type: '%s'",
                   ms->cpu_type);
        return;
    }

    /* if APIC ID is not set, set it based on socket/core/thread properties */
    if (cpu->apic_id == UNASSIGNED_APIC_ID) {
        int max_socket = (max_cpus - 1) / smp_threads / smp_cores;

        if (cpu->socket_id < 0) {
            error_setg(errp, "CPU socket-id is not set");
            return;
        } else if (cpu->socket_id > max_socket) {
            error_setg(errp, "Invalid CPU socket-id: %u must be in range 0:%u",
                       cpu->socket_id, max_socket);
            return;
        }
        if (cpu->core_id < 0) {
            error_setg(errp, "CPU core-id is not set");
            return;
        } else if (cpu->core_id > (smp_cores - 1)) {
            error_setg(errp, "Invalid CPU core-id: %u must be in range 0:%u",
                       cpu->core_id, smp_cores - 1);
            return;
        }
        if (cpu->thread_id < 0) {
            error_setg(errp, "CPU thread-id is not set");
            return;
        } else if (cpu->thread_id > (smp_threads - 1)) {
            error_setg(errp, "Invalid CPU thread-id: %u must be in range 0:%u",
                       cpu->thread_id, smp_threads - 1);
            return;
        }

        topo.pkg_id = cpu->socket_id;
        topo.core_id = cpu->core_id;
        topo.smt_id = cpu->thread_id;
        cpu->apic_id = apicid_from_topo_ids(smp_cores, smp_threads, &topo);
    }

    cpu_slot = cpu_find_slot(ms, cpu->apic_id, &idx);
    if (!cpu_slot) {
        x86_topo_ids_from_apicid(cpu->apic_id, smp_cores, smp_threads, &topo);
        error_setg(errp, "Invalid CPU [socket: %u, core: %u, thread: %u] with"
                  " APIC ID %" PRIu32 ", valid index range 0:%d",
                   topo.pkg_id, topo.core_id, topo.smt_id, cpu->apic_id,
                   ms->possible_cpus->len - 1);
        return;
    }

    if (cpu_slot->cpu) {
        error_setg(errp, "CPU[%d] with APIC ID %" PRIu32 " exists",
                   idx, cpu->apic_id);
        return;
    }

    /* if 'address' properties socket-id/core-id/thread-id are not set, set them
     * so that machine_query_hotpluggable_cpus would show correct values
     */
    /* TODO: move socket_id/core_id/thread_id checks into x86_cpu_realizefn()
     * once -smp refactoring is complete and there will be CPU private
     * CPUState::nr_cores and CPUState::nr_threads fields instead of globals */
    x86_topo_ids_from_apicid(cpu->apic_id, smp_cores, smp_threads, &topo);
    if (cpu->socket_id != -1 && cpu->socket_id != topo.pkg_id) {
        error_setg(errp, "property socket-id: %u doesn't match set apic-id:"
            " 0x%x (socket-id: %u)", cpu->socket_id, cpu->apic_id, topo.pkg_id);
        return;
    }
    cpu->socket_id = topo.pkg_id;

    if (cpu->core_id != -1 && cpu->core_id != topo.core_id) {
        error_setg(errp, "property core-id: %u doesn't match set apic-id:"
            " 0x%x (core-id: %u)", cpu->core_id, cpu->apic_id, topo.core_id);
        return;
    }
    cpu->core_id = topo.core_id;

    if (cpu->thread_id != -1 && cpu->thread_id != topo.smt_id) {
        error_setg(errp, "property thread-id: %u doesn't match set apic-id:"
            " 0x%x (thread-id: %u)", cpu->thread_id, cpu->apic_id, topo.smt_id);
        return;
    }
    cpu->thread_id = topo.smt_id;

    cs = CPU(cpu);
    cs->cpu_index = idx;

    numa_cpu_pre_plug(cpu_slot, dev, errp);
}

static void virt_machine_device_pre_plug_cb(HotplugHandler *hotplug_dev,
                                          DeviceState *dev, Error **errp)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        virt_cpu_pre_plug(hotplug_dev, dev, errp);
    }
}

static void virt_machine_device_plug_cb(HotplugHandler *hotplug_dev,
                                      DeviceState *dev, Error **errp)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        virt_cpu_plug(hotplug_dev, dev, errp);
    }
}

static HotplugHandler *virt_get_hotplug_handler(MachineState *machine,
                                             DeviceState *dev)
{
    VirtMachineClass *vmc = VIRT_MACHINE_GET_CLASS(machine);

    if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        return HOTPLUG_HANDLER(machine);
    }

    return vmc->orig_hotplug_handler ?
        vmc->orig_hotplug_handler(machine, dev) : NULL;
}


static void virt_machine_class_init(MachineClass *mc)
{
    VirtMachineClass *vmc = VIRT_MACHINE_CLASS(mc);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(mc);

    /* save original hotplug handler */
    vmc->orig_hotplug_handler = mc->get_hotplug_handler;

    mc->init = virt_machine_state_init;

    mc->family = "virt_i386";
    mc->desc = "Virtual i386 machine";
    mc->units_per_default_bus = 1;
    mc->no_floppy = 1;
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_AMD_IOMMU_DEVICE);
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_INTEL_IOMMU_DEVICE);
    machine_class_allow_dynamic_sysbus_dev(mc, "sysbus-debugcon");
    mc->max_cpus = 288;
    mc->has_hotpluggable_cpus = true;

    /* Machine class handlers */
    mc->cpu_index_to_instance_props = cpu_index_to_props;
    mc->get_default_cpu_node_id = cpu_get_default_cpu_node_id;
    mc->possible_cpu_arch_ids = cpu_possible_cpu_arch_ids;;
    mc->reset = virt_machine_reset;
    mc->get_hotplug_handler = virt_get_hotplug_handler;

    /* Hotplug handlers */
    hc->pre_plug = virt_machine_device_pre_plug_cb;
    hc->plug = virt_machine_device_plug_cb;

    /* Firmware building handler */
    mc->firmware_build_methods.acpi.madt = build_madt;
    mc->firmware_build_methods.acpi.rsdp = build_rsdp;
    mc->firmware_build_methods.acpi.setup = acpi_reduced_setup;
    mc->firmware_build_methods.acpi.mcfg = acpi_build_mcfg;
}

static void virt_2_12_machine_class_init(MachineClass *mc)
{
    virt_machine_class_init(mc);
    mc->alias = "virt";
}
DEFINE_VIRT_MACHINE_AS_LATEST(2, 12)
