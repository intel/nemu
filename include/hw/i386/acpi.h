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

#ifndef QEMU_I386_ACPI_H
#define QEMU_I386_ACPI_H

#include "hw/acpi/acpi_dev_interface.h"

/* MADT CPU callback */
void madt_cpu_entry(AcpiDeviceIf *adev, int uid,
                    const CPUArchIdList *apic_ids, GArray *entry);

#endif /* QEMU_I386_ACPI_H */
