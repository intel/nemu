/*
 * ACPI stubs for platforms that don't support ACPI.
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright (c) 2018 Intel, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/i386/pc_piix.h"
#include "hw/acpi/piix4.h"

void build_append_pcihp_notify_entry(Aml *method, int slot)
{}

void build_piix4_pm(Aml *table)
{}

void build_piix4_isa_bridge(Aml *table)
{}
 
void build_piix4_pci0_int(Aml *table)
{}

Object *piix4_pm_find(void)
{
    return NULL;
}
void build_piix4_pci_hotplug(Aml *table)
{}

