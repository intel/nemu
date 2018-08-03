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
#include "qemu/error-report.h"
#include "cpu.h"

#define VIRT_LOWMEM 0x80000000

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

    /* always allocate the device memory information */
    machine->device_memory = g_malloc0(sizeof(*machine->device_memory));

    /* initialize device memory address space */
    if (machine->ram_size < machine->maxram_size) {
        ram_addr_t device_mem_size =
            machine->maxram_size - machine->ram_size;

        if (machine->ram_slots > ACPI_MAX_RAM_SLOTS) {
            error_report("unsupported amount of memory slots: %"PRIu64,
                         machine->ram_slots);
            exit(EXIT_FAILURE);
        }

        if (QEMU_ALIGN_UP(machine->maxram_size,
                          TARGET_PAGE_SIZE) != machine->maxram_size) {
            error_report("maximum memory size must by aligned to multiple of "
                         "%d bytes", TARGET_PAGE_SIZE);
            exit(EXIT_FAILURE);
        }

        machine->device_memory->base =
            ROUND_UP(0x100000000ULL + highmem_size, 1ULL << 30);

        /* size hotplug region assuming 1G page max alignment per slot */
        device_mem_size += (1ULL << 30) * machine->ram_slots;

        if ((machine->device_memory->base + device_mem_size) <
            device_mem_size) {
            error_report("unsupported amount of maximum memory: " RAM_ADDR_FMT,
                         machine->maxram_size);
            exit(EXIT_FAILURE);
        }

        memory_region_init(&machine->device_memory->mr, OBJECT(vms),
                           "device-memory", device_mem_size);
        memory_region_add_subregion(system_memory, machine->device_memory->base,
                                    &machine->device_memory->mr);
    }

    sysfw_firmware_init(system_memory, false);
    
    return ram;
}
