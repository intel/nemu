/*
 * Copyright (c) 2018 Intel Corporation
 *
 * This code is licensed under the GNU GPLv2.
 * See the COPYING file in the top-level directory.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef HW_I386_SYSFW
#define HW_I386_SYSFW

#include "exec/memory.h"

#define BIOS_FILENAME "bios.bin"

void pc_system_flash_init(MemoryRegion *rom_memory);

#endif /* HW_I386_SYSFW */
