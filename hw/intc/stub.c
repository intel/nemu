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
#include "hw/i386/pc.h"

DeviceState *isa_pic=NULL;

int pic_get_output(DeviceState *d)
{
    return false;
}

int pic_read_irq(DeviceState *d)
{
    return 0;
}
