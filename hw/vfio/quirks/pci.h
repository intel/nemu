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

/*
 * The generic window quirks operate on an address and data register,
 * vfio_generic_window_address_quirk handles the address register and
 * vfio_generic_window_data_quirk handles the data register.  These ops
 * pass reads and writes through to hardware until a value matching the
 * stored address match/mask is written.  When this occurs, the data
 * register access emulated PCI config space for the device rather than
 * passing through accesses.  This enables devices where PCI config space
 * is accessible behind a window register to maintain the virtualization
 * provided through vfio.
 */
typedef struct VFIOConfigWindowMatch {
    uint32_t match;
    uint32_t mask;
} VFIOConfigWindowMatch;

typedef struct VFIOConfigWindowQuirk {
    struct VFIOPCIDevice *vdev;

    uint32_t address_val;

    uint32_t address_offset;
    uint32_t data_offset;

    bool window_enabled;
    uint8_t bar;

    MemoryRegion *addr_mem;
    MemoryRegion *data_mem;

    uint32_t nr_matches;
    VFIOConfigWindowMatch matches[];
} VFIOConfigWindowQuirk;

/*
 * The generic mirror quirk handles devices which expose PCI config space
 * through a region within a BAR.  When enabled, reads and writes are
 * redirected through to emulated PCI config space.  XXX if PCI config space
 * used memory regions, this could just be an alias.
 */
typedef struct VFIOConfigMirrorQuirk {
    struct VFIOPCIDevice *vdev;
    uint32_t offset;
    uint8_t bar;
    MemoryRegion *mem;
    uint8_t data[];
} VFIOConfigMirrorQuirk;

extern const MemoryRegionOps vfio_generic_window_address_quirk;
extern const MemoryRegionOps vfio_generic_window_data_quirk;
extern const MemoryRegionOps vfio_generic_mirror_quirk;

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
