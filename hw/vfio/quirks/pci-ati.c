/*
 * Device quirks for ATI PCI devices
 *
 * Copyright Red Hat, Inc. 2012-2015
 *
 * Authors:
 *  Alex Williamson <alex.williamson@redhat.com>
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
#include <sys/ioctl.h>
#include "hw/nvram/fw_cfg.h"
#include "pci.h"
#include "pci-ati.h"
#include "trace.h"

#define PCI_VENDOR_ID_ATI               0x1002

/*
 * Radeon HD cards (HD5450 & HD7850) report the upper byte of the I/O port BAR
 * through VGA register 0x3c3.  On newer cards, the I/O port BAR is always
 * BAR4 (older cards like the X550 used BAR1, but we don't care to support
 * those).  Note that on bare metal, a read of 0x3c3 doesn't always return the
 * I/O port BAR address.  Originally this was coded to return the virtual BAR
 * address only if the physical register read returns the actual BAR address,
 * but users have reported greater success if we return the virtual address
 * unconditionally.
 */
static uint64_t vfio_ati_3c3_quirk_read(void *opaque,
                                        hwaddr addr, unsigned size)
{
    VFIOPCIDevice *vdev = opaque;
    uint64_t data = vfio_pci_read_config(&vdev->pdev,
                                         PCI_BASE_ADDRESS_4 + 1, size);

    trace_vfio_quirk_ati_3c3_read(vdev->vbasedev.name, data);

    return data;
}

static const MemoryRegionOps vfio_ati_3c3_quirk = {
    .read = vfio_ati_3c3_quirk_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void vfio_vga_probe_ati_3c3_quirk(VFIOPCIDevice *vdev)
{
    VFIOQuirk *quirk;

    /*
     * As long as the BAR is >= 256 bytes it will be aligned such that the
     * lower byte is always zero.  Filter out anything else, if it exists.
     */
    if (!vfio_pci_is(vdev, PCI_VENDOR_ID_ATI, PCI_ANY_ID) ||
        !vdev->bars[4].ioport || vdev->bars[4].region.size < 256) {
        return;
    }

    quirk = vfio_quirk_alloc(1);

    memory_region_init_io(quirk->mem, OBJECT(vdev), &vfio_ati_3c3_quirk, vdev,
                          "vfio-ati-3c3-quirk", 1);
    memory_region_add_subregion(&vdev->vga->region[QEMU_PCI_VGA_IO_HI].mem,
                                3 /* offset 3 bytes from 0x3c0 */, quirk->mem);

    QLIST_INSERT_HEAD(&vdev->vga->region[QEMU_PCI_VGA_IO_HI].quirks,
                      quirk, next);

    trace_vfio_quirk_ati_3c3_probe(vdev->vbasedev.name);
}

/*
 * Newer ATI/AMD devices, including HD5450 and HD7850, have a mirror to PCI
 * config space through MMIO BAR2 at offset 0x4000.  Nothing seems to access
 * the MMIO space directly, but a window to this space is provided through
 * I/O port BAR4.  Offset 0x0 is the address register and offset 0x4 is the
 * data register.  When the address is programmed to a range of 0x4000-0x4fff
 * PCI configuration space is available.  Experimentation seems to indicate
 * that read-only may be provided by hardware.
 */
void vfio_probe_ati_bar4_quirk(VFIOPCIDevice *vdev, int nr)
{
    VFIOQuirk *quirk;
    VFIOConfigWindowQuirk *window;

    /* This windows doesn't seem to be used except by legacy VGA code */
    if (!vfio_pci_is(vdev, PCI_VENDOR_ID_ATI, PCI_ANY_ID) ||
        !vdev->vga || nr != 4) {
        return;
    }

    quirk = vfio_quirk_alloc(2);
    window = quirk->data = g_malloc0(sizeof(*window) +
                                     sizeof(VFIOConfigWindowMatch));
    window->vdev = vdev;
    window->address_offset = 0;
    window->data_offset = 4;
    window->nr_matches = 1;
    window->matches[0].match = 0x4000;
    window->matches[0].mask = vdev->config_size - 1;
    window->bar = nr;
    window->addr_mem = &quirk->mem[0];
    window->data_mem = &quirk->mem[1];

    memory_region_init_io(window->addr_mem, OBJECT(vdev),
                          &vfio_generic_window_address_quirk, window,
                          "vfio-ati-bar4-window-address-quirk", 4);
    memory_region_add_subregion_overlap(vdev->bars[nr].region.mem,
                                        window->address_offset,
                                        window->addr_mem, 1);

    memory_region_init_io(window->data_mem, OBJECT(vdev),
                          &vfio_generic_window_data_quirk, window,
                          "vfio-ati-bar4-window-data-quirk", 4);
    memory_region_add_subregion_overlap(vdev->bars[nr].region.mem,
                                        window->data_offset,
                                        window->data_mem, 1);

    QLIST_INSERT_HEAD(&vdev->bars[nr].quirks, quirk, next);

    trace_vfio_quirk_ati_bar4_probe(vdev->vbasedev.name);
}

/*
 * Trap the BAR2 MMIO mirror to config space as well.
 */
void vfio_probe_ati_bar2_quirk(VFIOPCIDevice *vdev, int nr)
{
    VFIOQuirk *quirk;
    VFIOConfigMirrorQuirk *mirror;

    /* Only enable on newer devices where BAR2 is 64bit */
    if (!vfio_pci_is(vdev, PCI_VENDOR_ID_ATI, PCI_ANY_ID) ||
        !vdev->vga || nr != 2 || !vdev->bars[2].mem64) {
        return;
    }

    quirk = vfio_quirk_alloc(1);
    mirror = quirk->data = g_malloc0(sizeof(*mirror));
    mirror->mem = quirk->mem;
    mirror->vdev = vdev;
    mirror->offset = 0x4000;
    mirror->bar = nr;

    memory_region_init_io(mirror->mem, OBJECT(vdev),
                          &vfio_generic_mirror_quirk, mirror,
                          "vfio-ati-bar2-4000-quirk", PCI_CONFIG_SPACE_SIZE);
    memory_region_add_subregion_overlap(vdev->bars[nr].region.mem,
                                        mirror->offset, mirror->mem, 1);

    QLIST_INSERT_HEAD(&vdev->bars[nr].quirks, quirk, next);

    trace_vfio_quirk_ati_bar2_probe(vdev->vbasedev.name);
}

/*
 * Older ATI/AMD cards like the X550 have a similar window to that above.
 * I/O port BAR1 provides a window to a mirror of PCI config space located
 * in BAR2 at offset 0xf00.  We don't care to support such older cards, but
 * note it for future reference.
 */


/*
 * AMD Radeon PCI config reset, based on Linux:
 *   drivers/gpu/drm/radeon/ci_smc.c:ci_is_smc_running()
 *   drivers/gpu/drm/radeon/radeon_device.c:radeon_pci_config_reset
 *   drivers/gpu/drm/radeon/ci_smc.c:ci_reset_smc()
 *   drivers/gpu/drm/radeon/ci_smc.c:ci_stop_smc_clock()
 * IDs: include/drm/drm_pciids.h
 * Registers: http://cgit.freedesktop.org/~agd5f/linux/commit/?id=4e2aa447f6f0
 *
 * Bonaire and Hawaii GPUs do not respond to a bus reset.  This is a bug in the
 * hardware that should be fixed on future ASICs.  The symptom of this is that
 * once the accerlated driver loads, Windows guests will bsod on subsequent
 * attmpts to load the driver, such as after VM reset or shutdown/restart.  To
 * work around this, we do an AMD specific PCI config reset, followed by an SMC
 * reset.  The PCI config reset only works if SMC firmware is running, so we
 * have a dependency on the state of the device as to whether this reset will
 * be effective.  There are still cases where we won't be able to kick the
 * device into working, but this greatly improves the usability overall.  The
 * config reset magic is relatively common on AMD GPUs, but the setup and SMC
 * poking is largely ASIC specific.
 */
static bool vfio_radeon_smc_is_running(VFIOPCIDevice *vdev)
{
    uint32_t clk, pc_c;

    /*
     * Registers 200h and 204h are index and data registers for accessing
     * indirect configuration registers within the device.
     */
    vfio_region_write(&vdev->bars[5].region, 0x200, 0x80000004, 4);
    clk = vfio_region_read(&vdev->bars[5].region, 0x204, 4);
    vfio_region_write(&vdev->bars[5].region, 0x200, 0x80000370, 4);
    pc_c = vfio_region_read(&vdev->bars[5].region, 0x204, 4);

    return (!(clk & 1) && (0x20100 <= pc_c));
}

/*
 * The scope of a config reset is controlled by a mode bit in the misc register
 * and a fuse, exposed as a bit in another register.  The fuse is the default
 * (0 = GFX, 1 = whole GPU), the misc bit is a toggle, with the forumula
 * scope = !(misc ^ fuse), where the resulting scope is defined the same as
 * the fuse.  A truth table therefore tells us that if misc == fuse, we need
 * to flip the value of the bit in the misc register.
 */
static void vfio_radeon_set_gfx_only_reset(VFIOPCIDevice *vdev)
{
    uint32_t misc, fuse;
    bool a, b;

    vfio_region_write(&vdev->bars[5].region, 0x200, 0xc00c0000, 4);
    fuse = vfio_region_read(&vdev->bars[5].region, 0x204, 4);
    b = fuse & 64;

    vfio_region_write(&vdev->bars[5].region, 0x200, 0xc0000010, 4);
    misc = vfio_region_read(&vdev->bars[5].region, 0x204, 4);
    a = misc & 2;

    if (a == b) {
        vfio_region_write(&vdev->bars[5].region, 0x204, misc ^ 2, 4);
        vfio_region_read(&vdev->bars[5].region, 0x204, 4); /* flush */
    }
}

int vfio_radeon_reset(VFIOPCIDevice *vdev)
{
    PCIDevice *pdev = &vdev->pdev;
    int i, ret = 0;
    uint32_t data;

    /* Defer to a kernel implemented reset */
    if (vdev->vbasedev.reset_works) {
        trace_vfio_quirk_ati_bonaire_reset_skipped(vdev->vbasedev.name);
        return -ENODEV;
    }

    /* Enable only memory BAR access */
    vfio_pci_write_config(pdev, PCI_COMMAND, PCI_COMMAND_MEMORY, 2);

    /* Reset only works if SMC firmware is loaded and running */
    if (!vfio_radeon_smc_is_running(vdev)) {
        ret = -EINVAL;
        trace_vfio_quirk_ati_bonaire_reset_no_smc(vdev->vbasedev.name);
        goto out;
    }

    /* Make sure only the GFX function is reset */
    vfio_radeon_set_gfx_only_reset(vdev);

    /* AMD PCI config reset */
    vfio_pci_write_config(pdev, 0x7c, 0x39d5e86b, 4);
    usleep(100);

    /* Read back the memory size to make sure we're out of reset */
    for (i = 0; i < 100000; i++) {
        if (vfio_region_read(&vdev->bars[5].region, 0x5428, 4) != 0xffffffff) {
            goto reset_smc;
        }
        usleep(1);
    }

    trace_vfio_quirk_ati_bonaire_reset_timeout(vdev->vbasedev.name);

reset_smc:
    /* Reset SMC */
    vfio_region_write(&vdev->bars[5].region, 0x200, 0x80000000, 4);
    data = vfio_region_read(&vdev->bars[5].region, 0x204, 4);
    data |= 1;
    vfio_region_write(&vdev->bars[5].region, 0x204, data, 4);

    /* Disable SMC clock */
    vfio_region_write(&vdev->bars[5].region, 0x200, 0x80000004, 4);
    data = vfio_region_read(&vdev->bars[5].region, 0x204, 4);
    data |= 1;
    vfio_region_write(&vdev->bars[5].region, 0x204, data, 4);

    trace_vfio_quirk_ati_bonaire_reset_done(vdev->vbasedev.name);

out:
    /* Restore PCI command register */
    vfio_pci_write_config(pdev, PCI_COMMAND, 0, 2);

    return ret;
}
