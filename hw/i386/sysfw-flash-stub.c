/*
 * Copyright (c) 2018 Intel Corporation
 *
 * This code is licensed under the GNU GPLv2.
 * See the COPYING file in the top-level directory.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include "qemu/osdep.h"
#include "exec/memory.h"
#include "sysfw.h"

void pc_system_flash_init(MemoryRegion *rom_memory)
{
    fprintf(stderr, "qemu: PFLASH boot unavailable\n");
    exit(1);
}

