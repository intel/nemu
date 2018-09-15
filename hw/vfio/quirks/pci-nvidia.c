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
#include "pci-nvidia.h"
#include "trace.h"

#define PCI_VENDOR_ID_NVIDIA                    0x10de

/*
 * Nvidia has several different methods to get to config space, the
 * nouveu project has several of these documented here:
 * https://github.com/pathscale/envytools/tree/master/hwdocs
 *
 * The first quirk is actually not documented in envytools and is found
 * on 10de:01d1 (NVIDIA Corporation G72 [GeForce 7300 LE]).  This is an
 * NV46 chipset.  The backdoor uses the legacy VGA I/O ports to access
 * the mirror of PCI config space found at BAR0 offset 0x1800.  The access
 * sequence first writes 0x338 to I/O port 0x3d4.  The target offset is
 * then written to 0x3d0.  Finally 0x538 is written for a read and 0x738
 * is written for a write to 0x3d4.  The BAR0 offset is then accessible
 * through 0x3d0.  This quirk doesn't seem to be necessary on newer cards
 * that use the I/O port BAR5 window but it doesn't hurt to leave it.
 */
typedef enum {NONE = 0, SELECT, WINDOW, READ, WRITE} VFIONvidia3d0State;
static const char *nv3d0_states[] = { "NONE", "SELECT",
                                      "WINDOW", "READ", "WRITE" };

typedef struct VFIONvidia3d0Quirk {
    VFIOPCIDevice *vdev;
    VFIONvidia3d0State state;
    uint32_t offset;
} VFIONvidia3d0Quirk;

static uint64_t vfio_nvidia_3d4_quirk_read(void *opaque,
                                           hwaddr addr, unsigned size)
{
    VFIONvidia3d0Quirk *quirk = opaque;
    VFIOPCIDevice *vdev = quirk->vdev;

    quirk->state = NONE;

    return vfio_vga_read(&vdev->vga->region[QEMU_PCI_VGA_IO_HI],
                         addr + 0x14, size);
}

static void vfio_nvidia_3d4_quirk_write(void *opaque, hwaddr addr,
                                        uint64_t data, unsigned size)
{
    VFIONvidia3d0Quirk *quirk = opaque;
    VFIOPCIDevice *vdev = quirk->vdev;
    VFIONvidia3d0State old_state = quirk->state;

    quirk->state = NONE;

    switch (data) {
    case 0x338:
        if (old_state == NONE) {
            quirk->state = SELECT;
            trace_vfio_quirk_nvidia_3d0_state(vdev->vbasedev.name,
                                              nv3d0_states[quirk->state]);
        }
        break;
    case 0x538:
        if (old_state == WINDOW) {
            quirk->state = READ;
            trace_vfio_quirk_nvidia_3d0_state(vdev->vbasedev.name,
                                              nv3d0_states[quirk->state]);
        }
        break;
    case 0x738:
        if (old_state == WINDOW) {
            quirk->state = WRITE;
            trace_vfio_quirk_nvidia_3d0_state(vdev->vbasedev.name,
                                              nv3d0_states[quirk->state]);
        }
        break;
    }

    vfio_vga_write(&vdev->vga->region[QEMU_PCI_VGA_IO_HI],
                   addr + 0x14, data, size);
}

static const MemoryRegionOps vfio_nvidia_3d4_quirk = {
    .read = vfio_nvidia_3d4_quirk_read,
    .write = vfio_nvidia_3d4_quirk_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t vfio_nvidia_3d0_quirk_read(void *opaque,
                                           hwaddr addr, unsigned size)
{
    VFIONvidia3d0Quirk *quirk = opaque;
    VFIOPCIDevice *vdev = quirk->vdev;
    VFIONvidia3d0State old_state = quirk->state;
    uint64_t data = vfio_vga_read(&vdev->vga->region[QEMU_PCI_VGA_IO_HI],
                                  addr + 0x10, size);

    quirk->state = NONE;

    if (old_state == READ &&
        (quirk->offset & ~(PCI_CONFIG_SPACE_SIZE - 1)) == 0x1800) {
        uint8_t offset = quirk->offset & (PCI_CONFIG_SPACE_SIZE - 1);

        data = vfio_pci_read_config(&vdev->pdev, offset, size);
        trace_vfio_quirk_nvidia_3d0_read(vdev->vbasedev.name,
                                         offset, size, data);
    }

    return data;
}

static void vfio_nvidia_3d0_quirk_write(void *opaque, hwaddr addr,
                                        uint64_t data, unsigned size)
{
    VFIONvidia3d0Quirk *quirk = opaque;
    VFIOPCIDevice *vdev = quirk->vdev;
    VFIONvidia3d0State old_state = quirk->state;

    quirk->state = NONE;

    if (old_state == SELECT) {
        quirk->offset = (uint32_t)data;
        quirk->state = WINDOW;
        trace_vfio_quirk_nvidia_3d0_state(vdev->vbasedev.name,
                                          nv3d0_states[quirk->state]);
    } else if (old_state == WRITE) {
        if ((quirk->offset & ~(PCI_CONFIG_SPACE_SIZE - 1)) == 0x1800) {
            uint8_t offset = quirk->offset & (PCI_CONFIG_SPACE_SIZE - 1);

            vfio_pci_write_config(&vdev->pdev, offset, data, size);
            trace_vfio_quirk_nvidia_3d0_write(vdev->vbasedev.name,
                                              offset, data, size);
            return;
        }
    }

    vfio_vga_write(&vdev->vga->region[QEMU_PCI_VGA_IO_HI],
                   addr + 0x10, data, size);
}

static const MemoryRegionOps vfio_nvidia_3d0_quirk = {
    .read = vfio_nvidia_3d0_quirk_read,
    .write = vfio_nvidia_3d0_quirk_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void vfio_vga_probe_nvidia_3d0_quirk(VFIOPCIDevice *vdev)
{
    VFIOQuirk *quirk;
    VFIONvidia3d0Quirk *data;

    if (vdev->no_geforce_quirks ||
        !vfio_pci_is(vdev, PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID) ||
        !vdev->bars[1].region.size) {
        return;
    }

    quirk = vfio_quirk_alloc(2);
    quirk->data = data = g_malloc0(sizeof(*data));
    data->vdev = vdev;

    memory_region_init_io(&quirk->mem[0], OBJECT(vdev), &vfio_nvidia_3d4_quirk,
                          data, "vfio-nvidia-3d4-quirk", 2);
    memory_region_add_subregion(&vdev->vga->region[QEMU_PCI_VGA_IO_HI].mem,
                                0x14 /* 0x3c0 + 0x14 */, &quirk->mem[0]);

    memory_region_init_io(&quirk->mem[1], OBJECT(vdev), &vfio_nvidia_3d0_quirk,
                          data, "vfio-nvidia-3d0-quirk", 2);
    memory_region_add_subregion(&vdev->vga->region[QEMU_PCI_VGA_IO_HI].mem,
                                0x10 /* 0x3c0 + 0x10 */, &quirk->mem[1]);

    QLIST_INSERT_HEAD(&vdev->vga->region[QEMU_PCI_VGA_IO_HI].quirks,
                      quirk, next);

    trace_vfio_quirk_nvidia_3d0_probe(vdev->vbasedev.name);
}

/*
 * The second quirk is documented in envytools.  The I/O port BAR5 is just
 * a set of address/data ports to the MMIO BARs.  The BAR we care about is
 * again BAR0.  This backdoor is apparently a bit newer than the one above
 * so we need to not only trap 256 bytes @0x1800, but all of PCI config
 * space, including extended space is available at the 4k @0x88000.
 */
typedef struct VFIONvidiaBAR5Quirk {
    uint32_t master;
    uint32_t enable;
    MemoryRegion *addr_mem;
    MemoryRegion *data_mem;
    bool enabled;
    VFIOConfigWindowQuirk window; /* last for match data */
} VFIONvidiaBAR5Quirk;

static void vfio_nvidia_bar5_enable(VFIONvidiaBAR5Quirk *bar5)
{
    VFIOPCIDevice *vdev = bar5->window.vdev;

    if (((bar5->master & bar5->enable) & 0x1) == bar5->enabled) {
        return;
    }

    bar5->enabled = !bar5->enabled;
    trace_vfio_quirk_nvidia_bar5_state(vdev->vbasedev.name,
                                       bar5->enabled ?  "Enable" : "Disable");
    memory_region_set_enabled(bar5->addr_mem, bar5->enabled);
    memory_region_set_enabled(bar5->data_mem, bar5->enabled);
}

static uint64_t vfio_nvidia_bar5_quirk_master_read(void *opaque,
                                                   hwaddr addr, unsigned size)
{
    VFIONvidiaBAR5Quirk *bar5 = opaque;
    VFIOPCIDevice *vdev = bar5->window.vdev;

    return vfio_region_read(&vdev->bars[5].region, addr, size);
}

static void vfio_nvidia_bar5_quirk_master_write(void *opaque, hwaddr addr,
                                                uint64_t data, unsigned size)
{
    VFIONvidiaBAR5Quirk *bar5 = opaque;
    VFIOPCIDevice *vdev = bar5->window.vdev;

    vfio_region_write(&vdev->bars[5].region, addr, data, size);

    bar5->master = data;
    vfio_nvidia_bar5_enable(bar5);
}

static const MemoryRegionOps vfio_nvidia_bar5_quirk_master = {
    .read = vfio_nvidia_bar5_quirk_master_read,
    .write = vfio_nvidia_bar5_quirk_master_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t vfio_nvidia_bar5_quirk_enable_read(void *opaque,
                                                   hwaddr addr, unsigned size)
{
    VFIONvidiaBAR5Quirk *bar5 = opaque;
    VFIOPCIDevice *vdev = bar5->window.vdev;

    return vfio_region_read(&vdev->bars[5].region, addr + 4, size);
}

static void vfio_nvidia_bar5_quirk_enable_write(void *opaque, hwaddr addr,
                                                uint64_t data, unsigned size)
{
    VFIONvidiaBAR5Quirk *bar5 = opaque;
    VFIOPCIDevice *vdev = bar5->window.vdev;

    vfio_region_write(&vdev->bars[5].region, addr + 4, data, size);

    bar5->enable = data;
    vfio_nvidia_bar5_enable(bar5);
}

static const MemoryRegionOps vfio_nvidia_bar5_quirk_enable = {
    .read = vfio_nvidia_bar5_quirk_enable_read,
    .write = vfio_nvidia_bar5_quirk_enable_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void vfio_probe_nvidia_bar5_quirk(VFIOPCIDevice *vdev, int nr)
{
    VFIOQuirk *quirk;
    VFIONvidiaBAR5Quirk *bar5;
    VFIOConfigWindowQuirk *window;

    if (vdev->no_geforce_quirks ||
        !vfio_pci_is(vdev, PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID) ||
        !vdev->vga || nr != 5 || !vdev->bars[5].ioport) {
        return;
    }

    quirk = vfio_quirk_alloc(4);
    bar5 = quirk->data = g_malloc0(sizeof(*bar5) +
                                   (sizeof(VFIOConfigWindowMatch) * 2));
    window = &bar5->window;

    window->vdev = vdev;
    window->address_offset = 0x8;
    window->data_offset = 0xc;
    window->nr_matches = 2;
    window->matches[0].match = 0x1800;
    window->matches[0].mask = PCI_CONFIG_SPACE_SIZE - 1;
    window->matches[1].match = 0x88000;
    window->matches[1].mask = vdev->config_size - 1;
    window->bar = nr;
    window->addr_mem = bar5->addr_mem = &quirk->mem[0];
    window->data_mem = bar5->data_mem = &quirk->mem[1];

    memory_region_init_io(window->addr_mem, OBJECT(vdev),
                          &vfio_generic_window_address_quirk, window,
                          "vfio-nvidia-bar5-window-address-quirk", 4);
    memory_region_add_subregion_overlap(vdev->bars[nr].region.mem,
                                        window->address_offset,
                                        window->addr_mem, 1);
    memory_region_set_enabled(window->addr_mem, false);

    memory_region_init_io(window->data_mem, OBJECT(vdev),
                          &vfio_generic_window_data_quirk, window,
                          "vfio-nvidia-bar5-window-data-quirk", 4);
    memory_region_add_subregion_overlap(vdev->bars[nr].region.mem,
                                        window->data_offset,
                                        window->data_mem, 1);
    memory_region_set_enabled(window->data_mem, false);

    memory_region_init_io(&quirk->mem[2], OBJECT(vdev),
                          &vfio_nvidia_bar5_quirk_master, bar5,
                          "vfio-nvidia-bar5-master-quirk", 4);
    memory_region_add_subregion_overlap(vdev->bars[nr].region.mem,
                                        0, &quirk->mem[2], 1);

    memory_region_init_io(&quirk->mem[3], OBJECT(vdev),
                          &vfio_nvidia_bar5_quirk_enable, bar5,
                          "vfio-nvidia-bar5-enable-quirk", 4);
    memory_region_add_subregion_overlap(vdev->bars[nr].region.mem,
                                        4, &quirk->mem[3], 1);

    QLIST_INSERT_HEAD(&vdev->bars[nr].quirks, quirk, next);

    trace_vfio_quirk_nvidia_bar5_probe(vdev->vbasedev.name);
}

typedef struct LastDataSet {
    VFIOQuirk *quirk;
    hwaddr addr;
    uint64_t data;
    unsigned size;
    int hits;
    int added;
} LastDataSet;

#define MAX_DYN_IOEVENTFD 10
#define HITS_FOR_IOEVENTFD 10

/*
 * Finally, BAR0 itself.  We want to redirect any accesses to either
 * 0x1800 or 0x88000 through the PCI config space access functions.
 */
static void vfio_nvidia_quirk_mirror_write(void *opaque, hwaddr addr,
                                           uint64_t data, unsigned size)
{
    VFIOConfigMirrorQuirk *mirror = opaque;
    VFIOPCIDevice *vdev = mirror->vdev;
    PCIDevice *pdev = &vdev->pdev;
    LastDataSet *last = (LastDataSet *)&mirror->data;

    vfio_generic_quirk_mirror_write(opaque, addr, data, size);

    /*
     * Nvidia seems to acknowledge MSI interrupts by writing 0xff to the
     * MSI capability ID register.  Both the ID and next register are
     * read-only, so we allow writes covering either of those to real hw.
     */
    if ((pdev->cap_present & QEMU_PCI_CAP_MSI) &&
        vfio_range_contained(addr, size, pdev->msi_cap, PCI_MSI_FLAGS)) {
        vfio_region_write(&vdev->bars[mirror->bar].region,
                          addr + mirror->offset, data, size);
        trace_vfio_quirk_nvidia_bar0_msi_ack(vdev->vbasedev.name);
    }

    /*
     * Automatically add an ioeventfd to handle any repeated write with the
     * same data and size above the standard PCI config space header.  This is
     * primarily expected to accelerate the MSI-ACK behavior, such as noted
     * above.  Current hardware/drivers should trigger an ioeventfd at config
     * offset 0x704 (region offset 0x88704), with data 0x0, size 4.
     *
     * The criteria of 10 successive hits is arbitrary but reliably adds the
     * MSI-ACK region.  Note that as some writes are bypassed via the ioeventfd,
     * the remaining ones have a greater chance of being seen successively.
     * To avoid the pathological case of burning up all of QEMU's open file
     * handles, arbitrarily limit this algorithm from adding no more than 10
     * ioeventfds, print an error if we would have added an 11th, and then
     * stop counting.
     */
    if (!vdev->no_kvm_ioeventfd &&
        addr >= PCI_STD_HEADER_SIZEOF && last->added <= MAX_DYN_IOEVENTFD) {
        if (addr != last->addr || data != last->data || size != last->size) {
            last->addr = addr;
            last->data = data;
            last->size = size;
            last->hits = 1;
        } else if (++last->hits >= HITS_FOR_IOEVENTFD) {
            if (last->added < MAX_DYN_IOEVENTFD) {
                VFIOIOEventFD *ioeventfd;
                ioeventfd = vfio_ioeventfd_init(vdev, mirror->mem, addr, size,
                                        data, &vdev->bars[mirror->bar].region,
                                        mirror->offset + addr, true);
                if (ioeventfd) {
                    VFIOQuirk *quirk = last->quirk;

                    QLIST_INSERT_HEAD(&quirk->ioeventfds, ioeventfd, next);
                    last->added++;
                }
            } else {
                last->added++;
                warn_report("NVIDIA ioeventfd queue full for %s, unable to "
                            "accelerate 0x%"HWADDR_PRIx", data 0x%"PRIx64", "
                            "size %u", vdev->vbasedev.name, addr, data, size);
            }
        }
    }
}

static const MemoryRegionOps vfio_nvidia_mirror_quirk = {
    .read = vfio_generic_quirk_mirror_read,
    .write = vfio_nvidia_quirk_mirror_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void vfio_nvidia_bar0_quirk_reset(VFIOPCIDevice *vdev, VFIOQuirk *quirk)
{
    VFIOConfigMirrorQuirk *mirror = quirk->data;
    LastDataSet *last = (LastDataSet *)&mirror->data;

    last->addr = last->data = last->size = last->hits = last->added = 0;

    vfio_drop_dynamic_eventfds(vdev, quirk);
}

void vfio_probe_nvidia_bar0_quirk(VFIOPCIDevice *vdev, int nr)
{
    VFIOQuirk *quirk;
    VFIOConfigMirrorQuirk *mirror;
    LastDataSet *last;

    if (vdev->no_geforce_quirks ||
        !vfio_pci_is(vdev, PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID) ||
        !vfio_is_vga(vdev) || nr != 0) {
        return;
    }

    quirk = vfio_quirk_alloc(1);
    quirk->reset = vfio_nvidia_bar0_quirk_reset;
    mirror = quirk->data = g_malloc0(sizeof(*mirror) + sizeof(LastDataSet));
    mirror->mem = quirk->mem;
    mirror->vdev = vdev;
    mirror->offset = 0x88000;
    mirror->bar = nr;
    last = (LastDataSet *)&mirror->data;
    last->quirk = quirk;

    memory_region_init_io(mirror->mem, OBJECT(vdev),
                          &vfio_nvidia_mirror_quirk, mirror,
                          "vfio-nvidia-bar0-88000-mirror-quirk",
                          vdev->config_size);
    memory_region_add_subregion_overlap(vdev->bars[nr].region.mem,
                                        mirror->offset, mirror->mem, 1);

    QLIST_INSERT_HEAD(&vdev->bars[nr].quirks, quirk, next);

    /* The 0x1800 offset mirror only seems to get used by legacy VGA */
    if (vdev->vga) {
        quirk = vfio_quirk_alloc(1);
        quirk->reset = vfio_nvidia_bar0_quirk_reset;
        mirror = quirk->data = g_malloc0(sizeof(*mirror) + sizeof(LastDataSet));
        mirror->mem = quirk->mem;
        mirror->vdev = vdev;
        mirror->offset = 0x1800;
        mirror->bar = nr;
        last = (LastDataSet *)&mirror->data;
        last->quirk = quirk;

        memory_region_init_io(mirror->mem, OBJECT(vdev),
                              &vfio_nvidia_mirror_quirk, mirror,
                              "vfio-nvidia-bar0-1800-mirror-quirk",
                              PCI_CONFIG_SPACE_SIZE);
        memory_region_add_subregion_overlap(vdev->bars[nr].region.mem,
                                            mirror->offset, mirror->mem, 1);

        QLIST_INSERT_HEAD(&vdev->bars[nr].quirks, quirk, next);
    }

    trace_vfio_quirk_nvidia_bar0_probe(vdev->vbasedev.name);
}

/*
 * TODO - Some Nvidia devices provide config access to their companion HDA
 * device and even to their parent bridge via these config space mirrors.
 * Add quirks for those regions.
 */

/*
 * The NVIDIA GPUDirect P2P Vendor capability allows the user to specify
 * devices as a member of a clique.  Devices within the same clique ID
 * are capable of direct P2P.  It's the user's responsibility that this
 * is correct.  The spec says that this may reside at any unused config
 * offset, but reserves and recommends hypervisors place this at C8h.
 * The spec also states that the hypervisor should place this capability
 * at the end of the capability list, thus next is defined as 0h.
 *
 * +----------------+----------------+----------------+----------------+
 * | sig 7:0 ('P')  |  vndr len (8h) |    next (0h)   |   cap id (9h)  |
 * +----------------+----------------+----------------+----------------+
 * | rsvd 15:7(0h),id 6:3,ver 2:0(0h)|          sig 23:8 ('P2')        |
 * +---------------------------------+---------------------------------+
 *
 * https://lists.gnu.org/archive/html/qemu-devel/2017-08/pdfUda5iEpgOS.pdf
 */
static void get_nv_gpudirect_clique_id(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    DeviceState *dev = DEVICE(obj);
    Property *prop = opaque;
    uint8_t *ptr = qdev_get_prop_ptr(dev, prop);

    visit_type_uint8(v, name, ptr, errp);
}

static void set_nv_gpudirect_clique_id(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    DeviceState *dev = DEVICE(obj);
    Property *prop = opaque;
    uint8_t value, *ptr = qdev_get_prop_ptr(dev, prop);
    Error *local_err = NULL;

    if (dev->realized) {
        qdev_prop_set_after_realize(dev, name, errp);
        return;
    }

    visit_type_uint8(v, name, &value, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    if (value & ~0xF) {
        error_setg(errp, "Property %s: valid range 0-15", name);
        return;
    }

    *ptr = value;
}

const PropertyInfo qdev_prop_nv_gpudirect_clique = {
    .name = "uint4",
    .description = "NVIDIA GPUDirect Clique ID (0 - 15)",
    .get = get_nv_gpudirect_clique_id,
    .set = set_nv_gpudirect_clique_id,
};

int vfio_add_nv_gpudirect_cap(VFIOPCIDevice *vdev, Error **errp)
{
    PCIDevice *pdev = &vdev->pdev;
    int ret, pos = 0xC8;

    if (vdev->nv_gpudirect_clique == 0xFF) {
        return 0;
    }

    if (!vfio_pci_is(vdev, PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID)) {
        error_setg(errp, "NVIDIA GPUDirect Clique ID: invalid device vendor");
        return -EINVAL;
    }

    if (pci_get_byte(pdev->config + PCI_CLASS_DEVICE + 1) !=
        PCI_BASE_CLASS_DISPLAY) {
        error_setg(errp, "NVIDIA GPUDirect Clique ID: unsupported PCI class");
        return -EINVAL;
    }

    ret = pci_add_capability(pdev, PCI_CAP_ID_VNDR, pos, 8, errp);
    if (ret < 0) {
        error_prepend(errp, "Failed to add NVIDIA GPUDirect cap: ");
        return ret;
    }

    memset(vdev->emulated_config_bits + pos, 0xFF, 8);
    pos += PCI_CAP_FLAGS;
    pci_set_byte(pdev->config + pos++, 8);
    pci_set_byte(pdev->config + pos++, 'P');
    pci_set_byte(pdev->config + pos++, '2');
    pci_set_byte(pdev->config + pos++, 'P');
    pci_set_byte(pdev->config + pos++, vdev->nv_gpudirect_clique << 3);
    pci_set_byte(pdev->config + pos, 0);

    return 0;
}
