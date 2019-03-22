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

#ifndef QEMU_I386_CPU_H
#define QEMU_I386_CPU_H

#include "hw/boards.h"

uint32_t cpu_apicid_from_index(unsigned int cpu_index, bool compat);

CpuInstanceProperties cpu_index_to_props(MachineState *ms, unsigned cpu_index);
int64_t cpu_get_default_cpu_node_id(const MachineState *ms, int idx);
const CPUArchIdList *cpu_possible_cpu_arch_ids(MachineState *ms);

void cpu_hot_add(const int64_t id, Error **errp);
uint32_t cpus_init(MachineState *ms, bool compat);

#endif
