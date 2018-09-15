/*
 * Copyright Intel (c), 2018
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */
#ifndef HW_VFIO_VFIO_QUIRKS_PCI_ATI_H
#define HW_VFIO_VFIO_QUIRKS_PCI_ATI_H

void vfio_vga_probe_ati_3c3_quirk(VFIOPCIDevice *vdev);
void vfio_probe_ati_bar4_quirk(VFIOPCIDevice *vdev, int nr);
void vfio_probe_ati_bar2_quirk(VFIOPCIDevice *vdev, int nr);
int vfio_radeon_reset(VFIOPCIDevice *vdev);

#endif
