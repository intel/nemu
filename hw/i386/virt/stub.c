/*
 * QEMU i386 virt Platform stub
 *
 * Copyright Intel Corp. 2018
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
//#include "qemu-common.h"
#include "sysemu/watchdog.h"
#include "hw/qdev-core.h"
#include "hw/i386/pc.h"
#include "./../../vfio/pci.h"
#include "hw/i386/apic_internal.h"

DeviceState *isa_pic=NULL;

int select_watchdog_action(const char *p)
{
    return -1;
}

int pic_get_output(DeviceState *d)
{
    return false;
}

void vfio_display_finalize(VFIOPCIDevice *vdev)
{
}

void vfio_display_reset(VFIOPCIDevice *vdev)
{
}

int vfio_display_probe(VFIOPCIDevice *vdev, Error **errp)
{
    return 0;
}

void vapic_report_tpr_access(DeviceState *dev, CPUState *cpu, target_ulong ip,
                                     TPRAccess access)
{
}

int pic_read_irq(DeviceState *d)
{
    return 0;
}
