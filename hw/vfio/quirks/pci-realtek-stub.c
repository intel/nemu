/*
 * VFIO quirks stubs for Realtek
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
#include "pci-realtek.h"
#include "trace.h"

void vfio_probe_rtl8168_bar2_quirk(VFIOPCIDevice *vdev, int nr)
{
    return;
}
