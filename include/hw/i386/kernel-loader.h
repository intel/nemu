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

#ifndef QEMU_I386_LOADER_H
#define QEMU_I386_LOADER_H

#include "hw/boards.h"

#include "hw/i386/acpi.h"
#include "hw/nvram/fw_cfg.h"

#include "cpu.h"

struct kernel_boot_info {
    uint64_t entry;
    bool protected_mode;
    bool long_mode;
};

extern struct kernel_boot_info boot_info;

void kernel_loader_reset_cpu(CPUX86State *env);
void kernel_loader_setup(void);

void load_linux_bzimage(MachineState *machine, AcpiConfiguration *conf, FWCfgState *fw_cfg);
void load_linux_efi(MachineState *machine);

#endif
