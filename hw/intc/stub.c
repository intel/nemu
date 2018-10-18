/*
 * QEMU hw stub
 *
 * Copyright Intel Corp. 2018
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "hw/qdev-core.h"
#include "hw/i386/apic.h"
#include "hw/i386/ioapic.h"

DeviceState *isa_pic=NULL;

int pic_get_output(DeviceState *d)
{
    return false;
}

int pic_read_irq(DeviceState *d)
{
    return 0;
}

void apic_poll_irq(DeviceState *dev)
{
}

void apic_sipi(DeviceState *dev)
{
}

int apic_accept_pic_intr(DeviceState *dev)
{
    return 0;
}

int apic_get_interrupt(DeviceState *dev)
{
    return -1;
}
