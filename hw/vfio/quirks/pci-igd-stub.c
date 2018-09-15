/*
 * VFIO quirks stubs for IGD
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
#include "pci-igd.h"
#include "trace.h"

void vfio_probe_igd_bar4_quirk(VFIOPCIDevice *vdev, int nr)
{
    return;
}

int vfio_pci_igd_opregion_init(VFIOPCIDevice *vdev,
                               struct vfio_region_info *info, Error **errp)
{
    return -1;
}
