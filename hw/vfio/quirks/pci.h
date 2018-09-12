/*
 * vfio based device assignment support - PCI devices
 *
 * Copyright Red Hat, Inc. 2012-2015
 *
 * Authors:
 *  Alex Williamson <alex.williamson@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */
#ifndef HW_VFIO_VFIO_QUIRKS_PCI_H
#define HW_VFIO_VFIO_QUIRKS_PCI_H

#include "hw/vfio/pci.h"

bool vfio_pci_is(VFIOPCIDevice *vdev, uint32_t vendor, uint32_t device);
bool vfio_is_vga(VFIOPCIDevice *vdev);
VFIOQuirk *vfio_quirk_alloc(int nr_mem);
bool vfio_blacklist_opt_rom(VFIOPCIDevice *vdev);
void vfio_vga_quirk_setup(VFIOPCIDevice *vdev);
void vfio_vga_quirk_exit(VFIOPCIDevice *vdev);
void vfio_vga_quirk_finalize(VFIOPCIDevice *vdev);
void vfio_bar_quirk_setup(VFIOPCIDevice *vdev, int nr);
void vfio_bar_quirk_exit(VFIOPCIDevice *vdev, int nr);
void vfio_bar_quirk_finalize(VFIOPCIDevice *vdev, int nr);
void vfio_setup_resetfn_quirk(VFIOPCIDevice *vdev);
int vfio_add_virt_caps(VFIOPCIDevice *vdev, Error **errp);
void vfio_quirk_reset(VFIOPCIDevice *vdev);
int vfio_pci_igd_opregion_init(VFIOPCIDevice *vdev,
                               struct vfio_region_info *info,
                               Error **errp);

#endif
