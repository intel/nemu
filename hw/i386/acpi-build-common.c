/* Support for generating ACPI tables and passing them to Guests
 *
 * Copyright (C) 2008-2010  Kevin O'Connor <kevin@koconnor.net>
 * Copyright (C) 2006 Fabrice Bellard
 * Copyright (C) 2013 Red Hat Inc
 *
 * Author: Michael S. Tsirkin <mst@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qmp/qnum.h"
#include "acpi-build.h"
#include "qemu-common.h"
#include "qemu/bitmap.h"
#include "qemu/error-report.h"
#include "hw/pci/pci.h"
#include "qom/cpu.h"
#include "target/i386/cpu.h"
#include "hw/misc/pvpanic.h"
#include "hw/timer/hpet.h"
#include "hw/acpi/acpi-defs.h"
#include "hw/acpi/acpi.h"
#include "hw/acpi/cpu.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/acpi/bios-linker-loader.h"
#include "hw/loader.h"
#include "hw/isa/isa.h"
#include "hw/block/fdc.h"
#include "hw/acpi/memory_hotplug.h"
#include "sysemu/tpm.h"
#include "hw/acpi/tpm.h"
#include "hw/acpi/vmgenid.h"
#include "sysemu/tpm_backend.h"
#include "hw/timer/mc146818rtc_regs.h"
#include "hw/mem/memory-device.h"
#include "sysemu/numa.h"

/* Supported chipsets: */
#include "hw/acpi/piix4.h"
#include "hw/acpi/pcihp.h"
#include "hw/i386/ich9.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci-host/q35.h"
#include "hw/i386/x86-iommu.h"

#include "hw/acpi/aml-build.h"

#include "qom/qom-qobject.h"
#include "hw/i386/amd_iommu.h"
#include "hw/i386/intel_iommu.h"

#include "hw/acpi/ipmi.h"

GArray *build_madt(GArray *table_data, BIOSLinker *linker, MachineState *ms, AcpiConfiguration *conf)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *apic_ids = mc->possible_cpu_arch_ids(ms);
    int madt_start = table_data->len;
    AcpiDeviceIfClass *adevc = ACPI_DEVICE_IF_GET_CLASS(conf->acpi_dev);
    AcpiDeviceIf *adev = ACPI_DEVICE_IF(conf->acpi_dev);
    bool x2apic_mode = false;

    AcpiMultipleApicTable *madt;
    AcpiMadtIoApic *io_apic;
    AcpiMadtIntsrcovr *intsrcovr;
    int i;

    madt = acpi_data_push(table_data, sizeof *madt);
    madt->local_apic_address = cpu_to_le32(APIC_DEFAULT_ADDRESS);
    madt->flags = cpu_to_le32(1);

    for (i = 0; i < apic_ids->len; i++) {
        adevc->madt_cpu(adev, i, apic_ids, table_data);
        if (apic_ids->cpus[i].arch_id > 254) {
            x2apic_mode = true;
        }
    }

    io_apic = acpi_data_push(table_data, sizeof *io_apic);
    io_apic->type = ACPI_APIC_IO;
    io_apic->length = sizeof(*io_apic);
    io_apic->io_apic_id = ACPI_BUILD_IOAPIC_ID;
    io_apic->address = cpu_to_le32(IO_APIC_DEFAULT_ADDRESS);
    io_apic->interrupt = cpu_to_le32(0);

    if (conf->apic_xrupt_override) {
        intsrcovr = acpi_data_push(table_data, sizeof *intsrcovr);
        intsrcovr->type   = ACPI_APIC_XRUPT_OVERRIDE;
        intsrcovr->length = sizeof(*intsrcovr);
        intsrcovr->source = 0;
        intsrcovr->gsi    = cpu_to_le32(2);
        intsrcovr->flags  = cpu_to_le16(0); /* conforms to bus specifications */
    }
    for (i = 1; i < 16; i++) {
#define ACPI_BUILD_PCI_IRQS ((1<<5) | (1<<9) | (1<<10) | (1<<11))
        if (!(ACPI_BUILD_PCI_IRQS & (1 << i))) {
            /* No need for a INT source override structure. */
            continue;
        }
        intsrcovr = acpi_data_push(table_data, sizeof *intsrcovr);
        intsrcovr->type   = ACPI_APIC_XRUPT_OVERRIDE;
        intsrcovr->length = sizeof(*intsrcovr);
        intsrcovr->source = i;
        intsrcovr->gsi    = cpu_to_le32(i);
        intsrcovr->flags  = cpu_to_le16(0xd); /* active high, level triggered */
    }

    if (x2apic_mode) {
        AcpiMadtLocalX2ApicNmi *local_nmi;

        local_nmi = acpi_data_push(table_data, sizeof *local_nmi);
        local_nmi->type   = ACPI_APIC_LOCAL_X2APIC_NMI;
        local_nmi->length = sizeof(*local_nmi);
        local_nmi->uid    = 0xFFFFFFFF; /* all processors */
        local_nmi->flags  = cpu_to_le16(0);
        local_nmi->lint   = 1; /* ACPI_LINT1 */
    } else {
        AcpiMadtLocalNmi *local_nmi;

        local_nmi = acpi_data_push(table_data, sizeof *local_nmi);
        local_nmi->type         = ACPI_APIC_LOCAL_NMI;
        local_nmi->length       = sizeof(*local_nmi);
        local_nmi->processor_id = 0xff; /* all processors */
        local_nmi->flags        = cpu_to_le16(0);
        local_nmi->lint         = 1; /* ACPI_LINT1 */
    }

    build_header(linker, table_data,
                 (void *)(table_data->data + madt_start), "APIC",
                 table_data->len - madt_start, 1, NULL, NULL);

    return table_data;
}
