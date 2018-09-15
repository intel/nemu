/*
 * VFIO quirks stubs for ATI
 *
 * Copyright Intel(c) 2018
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/range.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/nvram/fw_cfg.h"
#include "pci.h"
#include "pci-ati.h"
#include "trace.h"

void vfio_vga_probe_ati_3c3_quirk(VFIOPCIDevice *vdev)
{
    return;
}

void vfio_probe_ati_bar4_quirk(VFIOPCIDevice *vdev, int nr)
{
    return;
}

void vfio_probe_ati_bar2_quirk(VFIOPCIDevice *vdev, int nr)
{
    return;
}

int vfio_radeon_reset(VFIOPCIDevice *vdev)
{
    return 0;
}
