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
#include "exec/address-spaces.h"

#include "hw/boards.h"
#include "hw/i386/virt.h"
#include "hw/i386/memory.h"

/* 3GB split */
#define VIRT_LOWMEM 0xc0000000UL

MemoryRegion *virt_memory_init(VirtMachineState *vms)
{
    MachineState *machine = MACHINE(vms);  
    MemoryRegion *ram = g_new(MemoryRegion, 1), *lowmem, *highmem, *system_memory = get_system_memory();
    uint64_t lowmem_size, highmem_size;
    
    if (machine->ram_size >= VIRT_LOWMEM) {
        highmem_size = machine->ram_size - VIRT_LOWMEM;
        lowmem_size = VIRT_LOWMEM;
    } else {
        highmem_size = 0;
        lowmem_size = machine->ram_size;
    }
    
    memory_region_allocate_system_memory(ram, NULL, "virt.ram",
                                         machine->ram_size);

    lowmem = g_malloc(sizeof(*lowmem));
    memory_region_init_alias(lowmem, NULL, "lowmem", ram, 0, lowmem_size);
    memory_region_add_subregion(system_memory, 0, lowmem);
    e820_add_entry(0, lowmem_size, E820_RAM);

    if (highmem_size > 0) {
        highmem = g_malloc(sizeof(*highmem));
        memory_region_init_alias(highmem, NULL, "highmem", ram, lowmem_size, highmem_size);
        memory_region_add_subregion(system_memory, 0x100000000ULL, highmem);
        e820_add_entry(0x100000000ULL, highmem_size, E820_RAM);
    }
    
    return ram;
}
