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
#include "hw/i386/fw.h"

/* 3GB split */
#define VIRT_LOWMEM 0xc0000000UL

static void virt_memory_setup_cmos(VirtMachineState *vms) {
    uint64_t val;

    /* memory size */
    /* base memory (first MiB) */
    val = MIN(vms->below_4g_mem_size / 1024, 640);
    virt_cmos_set(vms->cmos, 0x15, val);
    virt_cmos_set(vms->cmos, 0x16, val >> 8);
    /* extended memory (next 64MiB) */
    if (vms->below_4g_mem_size > 1024 * 1024) {
        val = (vms->below_4g_mem_size - 1024 * 1024) / 1024;
    } else {
        val = 0;
    }
    if (val > 65535)
        val = 65535;
    virt_cmos_set(vms->cmos, 0x17, val);
    virt_cmos_set(vms->cmos, 0x18, val >> 8);
    virt_cmos_set(vms->cmos, 0x30, val);
    virt_cmos_set(vms->cmos, 0x31, val >> 8);
    /* memory between 16MiB and 4GiB */
    if (vms->below_4g_mem_size > 16 * 1024 * 1024) {
        val = (vms->below_4g_mem_size - 16 * 1024 * 1024) / 65536;
    } else {
        val = 0;
    }
    if (val > 65535)
        val = 65535;
    virt_cmos_set(vms->cmos, 0x34, val);
    virt_cmos_set(vms->cmos, 0x35, val >> 8);
    /* memory above 4GiB */
    val = vms->above_4g_mem_size / 65536;
    virt_cmos_set(vms->cmos, 0x5b, val);
    virt_cmos_set(vms->cmos, 0x5c, val >> 8);
    virt_cmos_set(vms->cmos, 0x5d, val >> 16);
}

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

    vms->below_4g_mem_size = lowmem_size;
    vms->above_4g_mem_size = highmem_size;

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

    virt_memory_setup_cmos(vms);

    if (vms->fw) {
        sysfw_firmware_init(system_memory, false);
    }
    
    return ram;
}
