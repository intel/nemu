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

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"

#include "sysemu/cpus.h"
#include "sysemu/qtest.h"
#include "sysemu/numa.h"
#include "sysemu/sysemu.h"

#include "hw/i386/cpu-internal.h"
#include "hw/i386/pc.h"
#include "hw/i386/topology.h"

#include "hw/acpi/pc-hotplug.h"

static void cpu_new(const char *typename, int64_t apic_id, Error **errp)
{
    Object *cpu = NULL;
    Error *local_err = NULL;

    cpu = object_new(typename);

    object_property_set_uint(cpu, apic_id, "apic-id", &local_err);
    object_property_set_bool(cpu, true, "realized", &local_err);

    object_unref(cpu);
    error_propagate(errp, local_err);
}

/* Calculates initial APIC ID for a specific CPU index
 *
 * Currently we need to be able to calculate the APIC ID from the CPU index
 * alone (without requiring a CPU object), as the QEMU<->Seabios interfaces have
 * no concept of "CPU index", and the NUMA tables on fw_cfg need the APIC ID of
 * all CPUs up to max_cpus.
 */
uint32_t cpu_apicid_from_index(unsigned int cpu_index, bool compat)
{
    uint32_t correct_id;
    static bool warned;

    return x86_apicid_from_cpu_idx(smp_cores, smp_threads, cpu_index);
    if (compat) {
        if (cpu_index != correct_id && !warned && !qtest_enabled()) {
            error_report("APIC IDs set in compatibility mode, "
                         "CPU topology won't match the configuration");
            warned = true;
        }
        return cpu_index;
    } else {
        return correct_id;
    }
}

CpuInstanceProperties cpu_index_to_props(MachineState *ms, unsigned cpu_index)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *possible_cpus = mc->possible_cpu_arch_ids(ms);

    assert(cpu_index < possible_cpus->len);
    return possible_cpus->cpus[cpu_index].props;
}


int64_t cpu_get_default_cpu_node_id(const MachineState *ms, int idx)
{
   X86CPUTopoInfo topo;

   assert(idx < ms->possible_cpus->len);
   x86_topo_ids_from_apicid(ms->possible_cpus->cpus[idx].arch_id,
                            smp_cores, smp_threads, &topo);
   return topo.pkg_id % nb_numa_nodes;
}

const CPUArchIdList *cpu_possible_cpu_arch_ids(MachineState *ms)
{
    int i;

    if (ms->possible_cpus) {
        /*
         * make sure that max_cpus hasn't changed since the first use, i.e.
         * -smp hasn't been parsed after it
        */
        assert(ms->possible_cpus->len == max_cpus);
        return ms->possible_cpus;
    }

    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * max_cpus);
    ms->possible_cpus->len = max_cpus;
    for (i = 0; i < ms->possible_cpus->len; i++) {
        X86CPUTopoInfo topo;

        ms->possible_cpus->cpus[i].type = ms->cpu_type;
        ms->possible_cpus->cpus[i].vcpus_count = 1;
        ms->possible_cpus->cpus[i].arch_id = cpu_apicid_from_index(i, compat_apic_id_mode);
        x86_topo_ids_from_apicid(ms->possible_cpus->cpus[i].arch_id,
                                 smp_cores, smp_threads, &topo);
        ms->possible_cpus->cpus[i].props.has_socket_id = true;
        ms->possible_cpus->cpus[i].props.socket_id = topo.pkg_id;
        ms->possible_cpus->cpus[i].props.has_core_id = true;
        ms->possible_cpus->cpus[i].props.core_id = topo.core_id;
        ms->possible_cpus->cpus[i].props.has_thread_id = true;
        ms->possible_cpus->cpus[i].props.thread_id = topo.smt_id;
    }
    return ms->possible_cpus;
}


void cpu_hot_add(const int64_t id, Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    int64_t apic_id = cpu_apicid_from_index(id, compat_apic_id_mode);
    Error *local_err = NULL;

    if (id < 0) {
        error_setg(errp, "Invalid CPU id: %" PRIi64, id);
        return;
    }

    if (apic_id >= ACPI_CPU_HOTPLUG_ID_LIMIT) {
        error_setg(errp, "Unable to add CPU: %" PRIi64
                   ", resulting APIC ID (%" PRIi64 ") is too large",
                   id, apic_id);
        return;
    }

    cpu_new(ms->cpu_type, apic_id, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
}


uint32_t cpus_init(MachineState *ms, bool compat)
{
    int i;
    uint32_t apic_id_limit;
    const CPUArchIdList *possible_cpus;
    MachineClass *mc = MACHINE_GET_CLASS(ms);

    /* Calculates the limit to CPU APIC ID values
     *
     * Limit for the APIC ID value, so that all
     * CPU APIC IDs are < ms->apic_id_limit.
     *
     * This is used for FW_CFG_MAX_CPUS. See comments on bochs_bios_init().
     */
    apic_id_limit = cpu_apicid_from_index(max_cpus - 1, compat) + 1;
    possible_cpus = mc->possible_cpu_arch_ids(ms);
    for (i = 0; i < smp_cpus; i++) {
        cpu_new(possible_cpus->cpus[i].type, possible_cpus->cpus[i].arch_id,
                &error_fatal);
    }

    return apic_id_limit;
}
