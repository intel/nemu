/*
 * QEMU Light weight PCI Host Bridge Emulation
 *
 * Copyright (C) 2016 Intel Corporation.
 *
 * Author:
 *  Chao Peng <chao.p.peng@linux.intel.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/i386/memory.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie_host.h"
#include "hw/pci-host/pci-lite.h"
#include "hw/isa/isa.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/range.h"
#include "hw/xen/xen.h"
#include "sysemu/sysemu.h"
#include "hw/i386/ioapic.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"

#define TYPE_PCI_LITE_HOST      "pci-lite-host"
#define TYPE_PCI_LITE_DEVICE    "pci-lite-device"

#define PCI_LITE_HOST(obj) \
    OBJECT_CHECK(PCILiteHost, (obj), TYPE_PCI_LITE_HOST)

#define PCI_LITE_NUM_IRQS       4
#define PCI_LITE_PCIEXBAR_BASE  0xb0000000
#define PCI_LITE_PCIEXBAR_SIZE  (0x100000) /* 1M for bus 0 */

#define DEFAULT_PCI_HOLE64_SIZE (~0x0ULL)

typedef struct PCILiteHost {
    /*< private >*/
    PCIExpressHost parent_obj;
    /*< public >*/

    Range pci_hole;
    Range pci_hole64;
    qemu_irq irq[PCI_LITE_NUM_IRQS];
    uint64_t pci_hole64_size;
} PCILiteHost;

static void pci_lite_get_pci_hole_start(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    PCILiteHost *s = PCI_LITE_HOST(obj);
    uint64_t val64;
    uint32_t value;

    val64 = range_is_empty(&s->pci_hole) ? 0 : range_lob(&s->pci_hole);
    value = val64;
    assert(value == val64);
    visit_type_uint32(v, name, &value, errp);
}

static void pci_lite_get_pci_hole_end(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    PCILiteHost *s = PCI_LITE_HOST(obj);
    uint64_t val64;
    uint32_t value;

    val64 = range_is_empty(&s->pci_hole) ? 0 : range_upb(&s->pci_hole) + 1;
    value = val64;
    assert(value == val64);
    visit_type_uint32(v, name, &value, errp);
}

static void pci_lite_get_pci_hole64_start(Object *obj, Visitor *v,
                                          const char *name,
                                          void *opaque, Error **errp)
{
    PCIHostState *h = PCI_HOST_BRIDGE(obj);
    Range w64;
    uint64_t value;

    pci_bus_get_w64_range(h->bus, &w64);
    value = range_is_empty(&w64) ? 0 : range_lob(&w64);
    if (!value) {
        value = ROUND_UP(0x100000000ULL, 1ULL << 30);
    }
    visit_type_uint64(v, name, &value, errp);
}

static void pci_lite_get_pci_hole64_end(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    PCIHostState *h = PCI_HOST_BRIDGE(obj);
    PCILiteHost *s = PCI_LITE_HOST(obj);
    uint64_t hole64_start = ROUND_UP(0x100000000ULL, 1ULL << 30);
    Range w64;
    uint64_t value, hole64_end;

    pci_bus_get_w64_range(h->bus, &w64);
    value = range_is_empty(&w64) ? 0 : range_upb(&w64) + 1;
    hole64_end = ROUND_UP(hole64_start + s->pci_hole64_size, 1ULL << 30);
    if (value < hole64_end) {
        value = hole64_end;
    }
    visit_type_uint64(v, name, &value, errp);
}

static void pci_lite_initfn(Object *obj)
{
    PCIHostState *s = PCI_HOST_BRIDGE(obj);

    memory_region_init_io(&s->conf_mem, obj, &pci_host_conf_le_ops, s,
                          "pci-conf-idx", 4);
    memory_region_init_io(&s->data_mem, obj, &pci_host_data_le_ops, s,
                          "pci-conf-data", 4);

    object_property_add(obj, PCI_HOST_PROP_PCI_HOLE_START, "int",
                        pci_lite_get_pci_hole_start,
                        NULL, NULL, NULL, NULL);

    object_property_add(obj, PCI_HOST_PROP_PCI_HOLE_END, "int",
                        pci_lite_get_pci_hole_end,
                        NULL, NULL, NULL, NULL);

    object_property_add(obj, PCI_HOST_PROP_PCI_HOLE64_START, "int",
                        pci_lite_get_pci_hole64_start,
                        NULL, NULL, NULL, NULL);

    object_property_add(obj, PCI_HOST_PROP_PCI_HOLE64_END, "int",
                        pci_lite_get_pci_hole64_end,
                        NULL, NULL, NULL, NULL);

}

static void pci_lite_set_irq(void *opaque, int irq_num, int level)
{
    PCILiteHost *d = opaque;

    qemu_set_irq(d->irq[irq_num], level);
}

static void pci_lite_realize(DeviceState *dev, Error **errp)
{
    PCIHostState *s = PCI_HOST_BRIDGE(dev);
    PCILiteHost *d = PCI_LITE_HOST(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    int i;

    sysbus_add_io(sbd, 0xcf8, &s->conf_mem);
    sysbus_init_ioports(sbd, 0xcf8, 4);

    sysbus_add_io(sbd, 0xcfc, &s->data_mem);
    sysbus_init_ioports(sbd, 0xcfc, 4);

    for (i = 0; i < PCI_LITE_NUM_IRQS; i++) {
        sysbus_init_irq(sbd, &d->irq[i]);
    }
}

PCIBus *pci_lite_init(MemoryRegion *address_space_mem,
                      MemoryRegion *address_space_io,
                      MemoryRegion *pci_address_space)
{
    DeviceState *dev;
    PCIHostState *pci;
    PCIExpressHost *pcie;
    PCILiteHost *pci_lite;

    dev = qdev_create(NULL, TYPE_PCI_LITE_HOST);
    pci = PCI_HOST_BRIDGE(dev);
    pcie = PCIE_HOST_BRIDGE(dev);

    pci->bus = pci_register_root_bus(dev, "pcie.0", pci_lite_set_irq,
                                pci_swizzle_map_irq_fn, pci, pci_address_space,
                                address_space_io, 0, 4, TYPE_PCIE_BUS);

    object_property_add_child(qdev_get_machine(), "pcilite", OBJECT(dev), NULL);
    qdev_init_nofail(dev);

    pci_lite = PCI_LITE_HOST(dev);

    range_set_bounds(&pci_lite->pci_hole,
                    PCI_LITE_PCIEXBAR_BASE + PCI_LITE_PCIEXBAR_SIZE,
                    IO_APIC_DEFAULT_ADDRESS);


    pcie_host_mmcfg_update(pcie, 1, PCI_LITE_PCIEXBAR_BASE,
                           PCI_LITE_PCIEXBAR_SIZE);
    e820_add_entry(PCI_LITE_PCIEXBAR_BASE, PCI_LITE_PCIEXBAR_SIZE,
                   E820_RESERVED);

    /* setup pci memory mapping */
    pc_pci_as_mapping_init(OBJECT(dev), address_space_mem, pci_address_space);

    pci_create_simple(pci->bus, 0, TYPE_PCI_LITE_DEVICE);
    return pci->bus;
}

static const char *pci_lite_root_bus_path(PCIHostState *host_bridge,
                                          PCIBus *rootbus)
{
    return "0000:00";
}

static Property pci_lite_props[] = {
    DEFINE_PROP_UINT64(PCIE_HOST_MCFG_BASE, PCILiteHost,
                       parent_obj.base_addr, PCI_LITE_PCIEXBAR_BASE),
    DEFINE_PROP_UINT64(PCIE_HOST_MCFG_SIZE, PCILiteHost,
                       parent_obj.size, PCI_LITE_PCIEXBAR_SIZE),
    DEFINE_PROP_SIZE(PCI_HOST_PROP_PCI_HOLE64_SIZE, PCILiteHost,
                     pci_hole64_size, DEFAULT_PCI_HOLE64_SIZE),
    DEFINE_PROP_END_OF_LIST(),
};

static void pci_lite_host_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->realize = pci_lite_realize;
    dc->props = pci_lite_props;
    hc->root_bus_path = pci_lite_root_bus_path;
}

static const TypeInfo pci_lite_host_info = {
    .name          = TYPE_PCI_LITE_HOST,
    .parent        = TYPE_PCIE_HOST_BRIDGE,
    .instance_size = sizeof(PCILiteHost),
    .instance_init = pci_lite_initfn,
    .class_init    = pci_lite_host_class_init,
};

static void pci_lite_device_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->desc = "Host bridge";

    // TODO: Use different one to GPEX?
    k->vendor_id = PCI_VENDOR_ID_REDHAT;
    k->device_id = PCI_DEVICE_ID_REDHAT_PCIE_HOST;
    k->revision = 0;

    /*
     * PCI-facing part of the host bridge, not usable without the
     * host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;
    dc->hotpluggable   = false;
}

static const TypeInfo pci_lite_device_info = {
    .name          = TYPE_PCI_LITE_DEVICE,
    .parent        = TYPE_PCI_DEVICE,
    .class_init    = pci_lite_device_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void pci_lite_register_types(void)
{
    type_register_static(&pci_lite_device_info);
    type_register_static(&pci_lite_host_info);
}

type_init(pci_lite_register_types)
