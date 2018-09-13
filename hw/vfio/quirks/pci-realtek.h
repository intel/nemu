/*
 * Copyright Intel (c), 2018
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#ifndef HW_VFIO_VFIO_QUIRKS_PCI_REALTEK_H
#define HW_VFIO_VFIO_QUIRKS_PCI_REALTEK_H

void vfio_probe_rtl8168_bar2_quirk(VFIOPCIDevice *vdev, int nr);

#endif
