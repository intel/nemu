/*
 * VFIO quirks stubs for NVIDIA
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
#include "pci-nvidia.h"
#include "trace.h"

void vfio_vga_probe_nvidia_3d0_quirk(VFIOPCIDevice *vdev)
{
    return;
}

void vfio_probe_nvidia_bar5_quirk(VFIOPCIDevice *vdev, int nr)
{
    return;
}

void vfio_probe_nvidia_bar0_quirk(VFIOPCIDevice *vdev, int nr)
{
    return;
}

int vfio_add_nv_gpudirect_cap(VFIOPCIDevice *vdev, Error **errp)

{
    return 0;
}

static void get_nv_gpudirect_clique_id(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    error_setg(errp, "Unsupported Nvidia clique ID property");
    return;
}

static void set_nv_gpudirect_clique_id(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    error_setg(errp, "Unsupported Nvidia clique ID property");
    return;
}

const PropertyInfo qdev_prop_nv_gpudirect_clique = {
    .name = "uint4",
    .description = "NVIDIA GPUDirect Clique ID (0 - 15)",
    .get = get_nv_gpudirect_clique_id,
    .set = set_nv_gpudirect_clique_id,
};
