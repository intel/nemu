/*
 * device quirks for PCI devices
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
#include "pci-igd.h"
#include "pci-ati.h"
#include "pci-nvidia.h"
#include "trace.h"

/* Use uin32_t for vendor & device so PCI_ANY_ID expands and cannot match hw */
bool vfio_pci_is(VFIOPCIDevice *vdev, uint32_t vendor, uint32_t device)
{
    return (vendor == PCI_ANY_ID || vendor == vdev->vendor_id) &&
           (device == PCI_ANY_ID || device == vdev->device_id);
}

bool vfio_is_vga(VFIOPCIDevice *vdev)
{
    PCIDevice *pdev = &vdev->pdev;
    uint16_t class = pci_get_word(pdev->config + PCI_CLASS_DEVICE);

    return class == PCI_CLASS_DISPLAY_VGA;
}

/*
 * List of device ids/vendor ids for which to disable
 * option rom loading. This avoids the guest hangs during rom
 * execution as noticed with the BCM 57810 card for lack of a
 * more better way to handle such issues.
 * The  user can still override by specifying a romfile or
 * rombar=1.
 * Please see https://bugs.launchpad.net/qemu/+bug/1284874
 * for an analysis of the 57810 card hang. When adding
 * a new vendor id/device id combination below, please also add
 * your card/environment details and information that could
 * help in debugging to the bug tracking this issue
 */
static const struct {
    uint32_t vendor;
    uint32_t device;
} romblacklist[] = {
    { 0x14e4, 0x168e }, /* Broadcom BCM 57810 */
};

bool vfio_blacklist_opt_rom(VFIOPCIDevice *vdev)
{
    int i;

    for (i = 0 ; i < ARRAY_SIZE(romblacklist); i++) {
        if (vfio_pci_is(vdev, romblacklist[i].vendor, romblacklist[i].device)) {
            trace_vfio_quirk_rom_blacklisted(vdev->vbasedev.name,
                                             romblacklist[i].vendor,
                                             romblacklist[i].device);
            return true;
        }
    }
    return false;
}

/*
 * Device specific region quirks (mostly backdoors to PCI config space)
 */

static uint64_t vfio_generic_window_quirk_address_read(void *opaque,
                                                hwaddr addr,
                                                unsigned size)
{
    VFIOConfigWindowQuirk *window = opaque;
    VFIOPCIDevice *vdev = window->vdev;

    return vfio_region_read(&vdev->bars[window->bar].region,
                            addr + window->address_offset, size);
}

static void vfio_generic_window_quirk_address_write(void *opaque, hwaddr addr,
                                                    uint64_t data,
                                                    unsigned size)
{
    VFIOConfigWindowQuirk *window = opaque;
    VFIOPCIDevice *vdev = window->vdev;
    int i;

    window->window_enabled = false;

    vfio_region_write(&vdev->bars[window->bar].region,
                      addr + window->address_offset, data, size);

    for (i = 0; i < window->nr_matches; i++) {
        if ((data & ~window->matches[i].mask) == window->matches[i].match) {
            window->window_enabled = true;
            window->address_val = data & window->matches[i].mask;
            trace_vfio_quirk_generic_window_address_write(vdev->vbasedev.name,
                                    memory_region_name(window->addr_mem), data);
            break;
        }
    }
}

const MemoryRegionOps vfio_generic_window_address_quirk = {
    .read = vfio_generic_window_quirk_address_read,
    .write = vfio_generic_window_quirk_address_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t vfio_generic_window_quirk_data_read(void *opaque,
                                                    hwaddr addr, unsigned size)
{
    VFIOConfigWindowQuirk *window = opaque;
    VFIOPCIDevice *vdev = window->vdev;
    uint64_t data;

    /* Always read data reg, discard if window enabled */
    data = vfio_region_read(&vdev->bars[window->bar].region,
                            addr + window->data_offset, size);

    if (window->window_enabled) {
        data = vfio_pci_read_config(&vdev->pdev, window->address_val, size);
        trace_vfio_quirk_generic_window_data_read(vdev->vbasedev.name,
                                    memory_region_name(window->data_mem), data);
    }

    return data;
}

static void vfio_generic_window_quirk_data_write(void *opaque, hwaddr addr,
                                                 uint64_t data, unsigned size)
{
    VFIOConfigWindowQuirk *window = opaque;
    VFIOPCIDevice *vdev = window->vdev;

    if (window->window_enabled) {
        vfio_pci_write_config(&vdev->pdev, window->address_val, data, size);
        trace_vfio_quirk_generic_window_data_write(vdev->vbasedev.name,
                                    memory_region_name(window->data_mem), data);
        return;
    }

    vfio_region_write(&vdev->bars[window->bar].region,
                      addr + window->data_offset, data, size);
}

const MemoryRegionOps vfio_generic_window_data_quirk = {
    .read = vfio_generic_window_quirk_data_read,
    .write = vfio_generic_window_quirk_data_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

uint64_t vfio_generic_quirk_mirror_read(void *opaque,
                                        hwaddr addr, unsigned size)
{
    VFIOConfigMirrorQuirk *mirror = opaque;
    VFIOPCIDevice *vdev = mirror->vdev;
    uint64_t data;

    /* Read and discard in case the hardware cares */
    (void)vfio_region_read(&vdev->bars[mirror->bar].region,
                           addr + mirror->offset, size);

    data = vfio_pci_read_config(&vdev->pdev, addr, size);
    trace_vfio_quirk_generic_mirror_read(vdev->vbasedev.name,
                                         memory_region_name(mirror->mem),
                                         addr, data);
    return data;
}

void vfio_generic_quirk_mirror_write(void *opaque, hwaddr addr,
                                     uint64_t data, unsigned size)
{
    VFIOConfigMirrorQuirk *mirror = opaque;
    VFIOPCIDevice *vdev = mirror->vdev;

    vfio_pci_write_config(&vdev->pdev, addr, data, size);
    trace_vfio_quirk_generic_mirror_write(vdev->vbasedev.name,
                                          memory_region_name(mirror->mem),
                                          addr, data);
}

const MemoryRegionOps vfio_generic_mirror_quirk = {
    .read = vfio_generic_quirk_mirror_read,
    .write = vfio_generic_quirk_mirror_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* Is range1 fully contained within range2?  */
bool vfio_range_contained(uint64_t first1, uint64_t len1,
                          uint64_t first2, uint64_t len2) {
    return (first1 >= first2 && first1 + len1 <= first2 + len2);
}

VFIOQuirk *vfio_quirk_alloc(int nr_mem)
{
    VFIOQuirk *quirk = g_new0(VFIOQuirk, 1);
    QLIST_INIT(&quirk->ioeventfds);
    quirk->mem = g_new0(MemoryRegion, nr_mem);
    quirk->nr_mem = nr_mem;

    return quirk;
}

void vfio_ioeventfd_exit(VFIOPCIDevice *vdev, VFIOIOEventFD *ioeventfd)
{
    QLIST_REMOVE(ioeventfd, next);
    memory_region_del_eventfd(ioeventfd->mr, ioeventfd->addr, ioeventfd->size,
                              true, ioeventfd->data, &ioeventfd->e);

    if (ioeventfd->vfio) {
        struct vfio_device_ioeventfd vfio_ioeventfd;

        vfio_ioeventfd.argsz = sizeof(vfio_ioeventfd);
        vfio_ioeventfd.flags = ioeventfd->size;
        vfio_ioeventfd.data = ioeventfd->data;
        vfio_ioeventfd.offset = ioeventfd->region->fd_offset +
                                ioeventfd->region_addr;
        vfio_ioeventfd.fd = -1;

        if (ioctl(vdev->vbasedev.fd, VFIO_DEVICE_IOEVENTFD, &vfio_ioeventfd)) {
            error_report("Failed to remove vfio ioeventfd for %s+0x%"
                         HWADDR_PRIx"[%d]:0x%"PRIx64" (%m)",
                         memory_region_name(ioeventfd->mr), ioeventfd->addr,
                         ioeventfd->size, ioeventfd->data);
        }
    } else {
        qemu_set_fd_handler(event_notifier_get_fd(&ioeventfd->e),
                            NULL, NULL, NULL);
    }

    event_notifier_cleanup(&ioeventfd->e);
    trace_vfio_ioeventfd_exit(memory_region_name(ioeventfd->mr),
                              (uint64_t)ioeventfd->addr, ioeventfd->size,
                              ioeventfd->data);
    g_free(ioeventfd);
}

void vfio_drop_dynamic_eventfds(VFIOPCIDevice *vdev, VFIOQuirk *quirk)
{
    VFIOIOEventFD *ioeventfd, *tmp;

    QLIST_FOREACH_SAFE(ioeventfd, &quirk->ioeventfds, next, tmp) {
        if (ioeventfd->dynamic) {
            vfio_ioeventfd_exit(vdev, ioeventfd);
        }
    }
}

static void vfio_ioeventfd_handler(void *opaque)
{
    VFIOIOEventFD *ioeventfd = opaque;

    if (event_notifier_test_and_clear(&ioeventfd->e)) {
        vfio_region_write(ioeventfd->region, ioeventfd->region_addr,
                          ioeventfd->data, ioeventfd->size);
        trace_vfio_ioeventfd_handler(memory_region_name(ioeventfd->mr),
                                     (uint64_t)ioeventfd->addr, ioeventfd->size,
                                     ioeventfd->data);
    }
}

VFIOIOEventFD *vfio_ioeventfd_init(VFIOPCIDevice *vdev,
                                   MemoryRegion *mr, hwaddr addr,
                                   unsigned size, uint64_t data,
                                   VFIORegion *region,
                                   hwaddr region_addr, bool dynamic)
{
    VFIOIOEventFD *ioeventfd;

    if (vdev->no_kvm_ioeventfd) {
        return NULL;
    }

    ioeventfd = g_malloc0(sizeof(*ioeventfd));

    if (event_notifier_init(&ioeventfd->e, 0)) {
        g_free(ioeventfd);
        return NULL;
    }

    /*
     * MemoryRegion and relative offset, plus additional ioeventfd setup
     * parameters for configuring and later tearing down KVM ioeventfd.
     */
    ioeventfd->mr = mr;
    ioeventfd->addr = addr;
    ioeventfd->size = size;
    ioeventfd->data = data;
    ioeventfd->dynamic = dynamic;
    /*
     * VFIORegion and relative offset for implementing the userspace
     * handler.  data & size fields shared for both uses.
     */
    ioeventfd->region = region;
    ioeventfd->region_addr = region_addr;

    if (!vdev->no_vfio_ioeventfd) {
        struct vfio_device_ioeventfd vfio_ioeventfd;

        vfio_ioeventfd.argsz = sizeof(vfio_ioeventfd);
        vfio_ioeventfd.flags = ioeventfd->size;
        vfio_ioeventfd.data = ioeventfd->data;
        vfio_ioeventfd.offset = ioeventfd->region->fd_offset +
                                ioeventfd->region_addr;
        vfio_ioeventfd.fd = event_notifier_get_fd(&ioeventfd->e);

        ioeventfd->vfio = !ioctl(vdev->vbasedev.fd,
                                 VFIO_DEVICE_IOEVENTFD, &vfio_ioeventfd);
    }

    if (!ioeventfd->vfio) {
        qemu_set_fd_handler(event_notifier_get_fd(&ioeventfd->e),
                            vfio_ioeventfd_handler, NULL, ioeventfd);
    }

    memory_region_add_eventfd(ioeventfd->mr, ioeventfd->addr, ioeventfd->size,
                              true, ioeventfd->data, &ioeventfd->e);
    trace_vfio_ioeventfd_init(memory_region_name(mr), (uint64_t)addr,
                              size, data, ioeventfd->vfio);

    return ioeventfd;
}

#define PCI_VENDOR_ID_REALTEK 0x10ec

/*
 * RTL8168 devices have a backdoor that can access the MSI-X table.  At BAR2
 * offset 0x70 there is a dword data register, offset 0x74 is a dword address
 * register.  According to the Linux r8169 driver, the MSI-X table is addressed
 * when the "type" portion of the address register is set to 0x1.  This appears
 * to be bits 16:30.  Bit 31 is both a write indicator and some sort of
 * "address latched" indicator.  Bits 12:15 are a mask field, which we can
 * ignore because the MSI-X table should always be accessed as a dword (full
 * mask).  Bits 0:11 is offset within the type.
 *
 * Example trace:
 *
 * Read from MSI-X table offset 0
 * vfio: vfio_bar_write(0000:05:00.0:BAR2+0x74, 0x1f000, 4) // store read addr
 * vfio: vfio_bar_read(0000:05:00.0:BAR2+0x74, 4) = 0x8001f000 // latch
 * vfio: vfio_bar_read(0000:05:00.0:BAR2+0x70, 4) = 0xfee00398 // read data
 *
 * Write 0xfee00000 to MSI-X table offset 0
 * vfio: vfio_bar_write(0000:05:00.0:BAR2+0x70, 0xfee00000, 4) // write data
 * vfio: vfio_bar_write(0000:05:00.0:BAR2+0x74, 0x8001f000, 4) // do write
 * vfio: vfio_bar_read(0000:05:00.0:BAR2+0x74, 4) = 0x1f000 // complete
 */
typedef struct VFIOrtl8168Quirk {
    VFIOPCIDevice *vdev;
    uint32_t addr;
    uint32_t data;
    bool enabled;
} VFIOrtl8168Quirk;

static uint64_t vfio_rtl8168_quirk_address_read(void *opaque,
                                                hwaddr addr, unsigned size)
{
    VFIOrtl8168Quirk *rtl = opaque;
    VFIOPCIDevice *vdev = rtl->vdev;
    uint64_t data = vfio_region_read(&vdev->bars[2].region, addr + 0x74, size);

    if (rtl->enabled) {
        data = rtl->addr ^ 0x80000000U; /* latch/complete */
        trace_vfio_quirk_rtl8168_fake_latch(vdev->vbasedev.name, data);
    }

    return data;
}

static void vfio_rtl8168_quirk_address_write(void *opaque, hwaddr addr,
                                             uint64_t data, unsigned size)
{
    VFIOrtl8168Quirk *rtl = opaque;
    VFIOPCIDevice *vdev = rtl->vdev;

    rtl->enabled = false;

    if ((data & 0x7fff0000) == 0x10000) { /* MSI-X table */
        rtl->enabled = true;
        rtl->addr = (uint32_t)data;

        if (data & 0x80000000U) { /* Do write */
            if (vdev->pdev.cap_present & QEMU_PCI_CAP_MSIX) {
                hwaddr offset = data & 0xfff;
                uint64_t val = rtl->data;

                trace_vfio_quirk_rtl8168_msix_write(vdev->vbasedev.name,
                                                    (uint16_t)offset, val);

                /* Write to the proper guest MSI-X table instead */
                memory_region_dispatch_write(&vdev->pdev.msix_table_mmio,
                                             offset, val, size,
                                             MEMTXATTRS_UNSPECIFIED);
            }
            return; /* Do not write guest MSI-X data to hardware */
        }
    }

    vfio_region_write(&vdev->bars[2].region, addr + 0x74, data, size);
}

static const MemoryRegionOps vfio_rtl_address_quirk = {
    .read = vfio_rtl8168_quirk_address_read,
    .write = vfio_rtl8168_quirk_address_write,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t vfio_rtl8168_quirk_data_read(void *opaque,
                                             hwaddr addr, unsigned size)
{
    VFIOrtl8168Quirk *rtl = opaque;
    VFIOPCIDevice *vdev = rtl->vdev;
    uint64_t data = vfio_region_read(&vdev->bars[2].region, addr + 0x70, size);

    if (rtl->enabled && (vdev->pdev.cap_present & QEMU_PCI_CAP_MSIX)) {
        hwaddr offset = rtl->addr & 0xfff;
        memory_region_dispatch_read(&vdev->pdev.msix_table_mmio, offset,
                                    &data, size, MEMTXATTRS_UNSPECIFIED);
        trace_vfio_quirk_rtl8168_msix_read(vdev->vbasedev.name, offset, data);
    }

    return data;
}

static void vfio_rtl8168_quirk_data_write(void *opaque, hwaddr addr,
                                          uint64_t data, unsigned size)
{
    VFIOrtl8168Quirk *rtl = opaque;
    VFIOPCIDevice *vdev = rtl->vdev;

    rtl->data = (uint32_t)data;

    vfio_region_write(&vdev->bars[2].region, addr + 0x70, data, size);
}

static const MemoryRegionOps vfio_rtl_data_quirk = {
    .read = vfio_rtl8168_quirk_data_read,
    .write = vfio_rtl8168_quirk_data_write,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void vfio_probe_rtl8168_bar2_quirk(VFIOPCIDevice *vdev, int nr)
{
    VFIOQuirk *quirk;
    VFIOrtl8168Quirk *rtl;

    if (!vfio_pci_is(vdev, PCI_VENDOR_ID_REALTEK, 0x8168) || nr != 2) {
        return;
    }

    quirk = vfio_quirk_alloc(2);
    quirk->data = rtl = g_malloc0(sizeof(*rtl));
    rtl->vdev = vdev;

    memory_region_init_io(&quirk->mem[0], OBJECT(vdev),
                          &vfio_rtl_address_quirk, rtl,
                          "vfio-rtl8168-window-address-quirk", 4);
    memory_region_add_subregion_overlap(vdev->bars[nr].region.mem,
                                        0x74, &quirk->mem[0], 1);

    memory_region_init_io(&quirk->mem[1], OBJECT(vdev),
                          &vfio_rtl_data_quirk, rtl,
                          "vfio-rtl8168-window-data-quirk", 4);
    memory_region_add_subregion_overlap(vdev->bars[nr].region.mem,
                                        0x70, &quirk->mem[1], 1);

    QLIST_INSERT_HEAD(&vdev->bars[nr].quirks, quirk, next);

    trace_vfio_quirk_rtl8168_probe(vdev->vbasedev.name);
}

/*
 * Common quirk probe entry points.
 */
void vfio_vga_quirk_setup(VFIOPCIDevice *vdev)
{
    vfio_vga_probe_ati_3c3_quirk(vdev);
    vfio_vga_probe_nvidia_3d0_quirk(vdev);
}

void vfio_vga_quirk_exit(VFIOPCIDevice *vdev)
{
    VFIOQuirk *quirk;
    int i, j;

    for (i = 0; i < ARRAY_SIZE(vdev->vga->region); i++) {
        QLIST_FOREACH(quirk, &vdev->vga->region[i].quirks, next) {
            for (j = 0; j < quirk->nr_mem; j++) {
                memory_region_del_subregion(&vdev->vga->region[i].mem,
                                            &quirk->mem[j]);
            }
        }
    }
}

void vfio_vga_quirk_finalize(VFIOPCIDevice *vdev)
{
    int i, j;

    for (i = 0; i < ARRAY_SIZE(vdev->vga->region); i++) {
        while (!QLIST_EMPTY(&vdev->vga->region[i].quirks)) {
            VFIOQuirk *quirk = QLIST_FIRST(&vdev->vga->region[i].quirks);
            QLIST_REMOVE(quirk, next);
            for (j = 0; j < quirk->nr_mem; j++) {
                object_unparent(OBJECT(&quirk->mem[j]));
            }
            g_free(quirk->mem);
            g_free(quirk->data);
            g_free(quirk);
        }
    }
}

void vfio_bar_quirk_setup(VFIOPCIDevice *vdev, int nr)
{
    vfio_probe_ati_bar4_quirk(vdev, nr);
    vfio_probe_ati_bar2_quirk(vdev, nr);
    vfio_probe_nvidia_bar5_quirk(vdev, nr);
    vfio_probe_nvidia_bar0_quirk(vdev, nr);
    vfio_probe_rtl8168_bar2_quirk(vdev, nr);
    vfio_probe_igd_bar4_quirk(vdev, nr);
}

void vfio_bar_quirk_exit(VFIOPCIDevice *vdev, int nr)
{
    VFIOBAR *bar = &vdev->bars[nr];
    VFIOQuirk *quirk;
    int i;

    QLIST_FOREACH(quirk, &bar->quirks, next) {
        while (!QLIST_EMPTY(&quirk->ioeventfds)) {
            vfio_ioeventfd_exit(vdev, QLIST_FIRST(&quirk->ioeventfds));
        }

        for (i = 0; i < quirk->nr_mem; i++) {
            memory_region_del_subregion(bar->region.mem, &quirk->mem[i]);
        }
    }
}

void vfio_bar_quirk_finalize(VFIOPCIDevice *vdev, int nr)
{
    VFIOBAR *bar = &vdev->bars[nr];
    int i;

    while (!QLIST_EMPTY(&bar->quirks)) {
        VFIOQuirk *quirk = QLIST_FIRST(&bar->quirks);
        QLIST_REMOVE(quirk, next);
        for (i = 0; i < quirk->nr_mem; i++) {
            object_unparent(OBJECT(&quirk->mem[i]));
        }
        g_free(quirk->mem);
        g_free(quirk->data);
        g_free(quirk);
    }
}

/*
 * Reset quirks
 */
void vfio_quirk_reset(VFIOPCIDevice *vdev)
{
    int i;

    for (i = 0; i < PCI_ROM_SLOT; i++) {
        VFIOQuirk *quirk;
        VFIOBAR *bar = &vdev->bars[i];

        QLIST_FOREACH(quirk, &bar->quirks, next) {
            if (quirk->reset) {
                quirk->reset(vdev, quirk);
            }
        }
    }
}

void vfio_setup_resetfn_quirk(VFIOPCIDevice *vdev)
{
    switch (vdev->vendor_id) {
    case 0x1002:
        switch (vdev->device_id) {
        /* Bonaire */
        case 0x6649: /* Bonaire [FirePro W5100] */
        case 0x6650:
        case 0x6651:
        case 0x6658: /* Bonaire XTX [Radeon R7 260X] */
        case 0x665c: /* Bonaire XT [Radeon HD 7790/8770 / R9 260 OEM] */
        case 0x665d: /* Bonaire [Radeon R7 200 Series] */
        /* Hawaii */
        case 0x67A0: /* Hawaii XT GL [FirePro W9100] */
        case 0x67A1: /* Hawaii PRO GL [FirePro W8100] */
        case 0x67A2:
        case 0x67A8:
        case 0x67A9:
        case 0x67AA:
        case 0x67B0: /* Hawaii XT [Radeon R9 290X] */
        case 0x67B1: /* Hawaii PRO [Radeon R9 290] */
        case 0x67B8:
        case 0x67B9:
        case 0x67BA:
        case 0x67BE:
            vdev->resetfn = vfio_radeon_reset;
            trace_vfio_quirk_ati_bonaire_reset(vdev->vbasedev.name);
            break;
        }
        break;
    }
}

int vfio_add_virt_caps(VFIOPCIDevice *vdev, Error **errp)
{
    int ret;

    ret = vfio_add_nv_gpudirect_cap(vdev, errp);
    if (ret) {
        return ret;
    }

    return 0;
}
