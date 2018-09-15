/*
 * Copyright Intel (c), 2018
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#ifndef HW_VFIO_VFIO_QUIRKS_PCI_NVIDIA_H
#define HW_VFIO_VFIO_QUIRKS_PCI_NVIDIA_H

void vfio_vga_probe_nvidia_3d0_quirk(VFIOPCIDevice *vdev);
void vfio_probe_nvidia_bar5_quirk(VFIOPCIDevice *vdev, int nr);
void vfio_probe_nvidia_bar0_quirk(VFIOPCIDevice *vdev, int nr);
int vfio_add_nv_gpudirect_cap(VFIOPCIDevice *vdev, Error **errp);

#endif
