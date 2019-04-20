/*
 * QEMU i386 vfio stub
 *
 * Copyright Intel Corp. 2018
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "pci.h"

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
