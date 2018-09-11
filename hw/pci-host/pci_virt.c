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
#include "hw/pci-host/pci-virt.h"
#include "hw/isa/isa.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/range.h"
#include "hw/xen/xen.h"
#include "sysemu/sysemu.h"
#include "hw/i386/ioapic.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "hw/i386/virt.h"
#include "hw/virtio/virtio-pci.h"

#define TYPE_PCI_VIRT_HOST      "pci-virt-host"
#define TYPE_PCI_VIRT_DEVICE    "pci-virt-device"

#define PCI_VIRT_HOST(obj) \
    OBJECT_CHECK(PCIVirtHost, (obj), TYPE_PCI_VIRT_HOST)

#define PCI_VIRT_NUM_IRQS       4            /* TODO: MSI only */

//TODO: Till we fix DMI place it below the current bar base
//Longer term this has to be calculated dynamically based
//on the location of the segment
#define PCI_VIRT_PCIEXBAR_BASE    (0x60000000)
#define PCI_VIRT_HOLE_START_BASE  (0x70000000)

//OPEN: Will the scan logic fail if it does not see the full 256MB
//Allocate just enough for the scan of bus 0 to complete
#define PCI_VIRT_PCIEXBAR_SIZE    (32*8*4096)

//TODO: Place it right after the main PCI hole, pick a safe number
//for now that does not conflict with segment 0
#define PCI_VIRT_HOLE64_START_BASE 0x900000000ULL 

//Define this to be the upper bound of what is required for PCI devices
//#define DEFAULT_PCI_HOLE64_SIZE (1ULL << 35) 
#define DEFAULT_PCI_HOLE64_SIZE    (1ULL << 22)


typedef struct PCIVirtHost {
    /*< private >*/
    PCIExpressHost parent_obj;
    /*< public >*/

    Range pci_hole;
    Range pci_hole64;
    uint64_t pci_hole64_size;

    qemu_irq irq[PCI_VIRT_NUM_IRQS]; //TODO: MSI Only
} PCIVirtHost;

/*
 * The 64bit pci hole starts after "above 4G RAM" and
 * potentially the space reserved for memory hotplug.
 * and beyond the main PCI hole
 * TODO: How to make this more generic when we have more segments
 * Also this is cumulative. 
 */
static uint64_t pci_virt_pci_hole64_start(void)
{
    //VirtMachineState *vms = VIRT_MACHINE(qdev_get_machine());
    uint64_t hole64_start = 0;

    //TODO: These holes need to placed dynamically
    hole64_start = PCI_VIRT_HOLE64_START_BASE;
    return hole64_start;
}


// We cannot use the lower 4GB. Can QEMU Handle returning 0
// for start and end
static void pci_virt_get_pci_hole_start(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    PCIVirtHost *s = PCI_VIRT_HOST(obj);
    uint64_t val64;
    uint32_t value;

    val64 = range_is_empty(&s->pci_hole) ? 0 : range_lob(&s->pci_hole);
    value = val64;
    assert(value == val64);
    visit_type_uint32(v, name, &value, errp);
}

static void pci_virt_get_pci_hole_end(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    PCIVirtHost *s = PCI_VIRT_HOST(obj);
    uint64_t val64;
    uint32_t value;

    val64 = range_is_empty(&s->pci_hole) ? 0 : range_upb(&s->pci_hole) + 1;
    value = val64;
    assert(value == val64);
    visit_type_uint32(v, name, &value, errp);
}

static void pci_virt_get_pci_hole64_start(Object *obj, Visitor *v,
                                          const char *name,
                                          void *opaque, Error **errp)
{
    PCIHostState *h = PCI_HOST_BRIDGE(obj);
    Range w64;
    uint64_t value;

    pci_bus_get_w64_range(h->bus, &w64);
    value = range_is_empty(&w64) ? 0 : range_lob(&w64);
    if (!value) {
        value = pci_virt_pci_hole64_start();
    }
    visit_type_uint64(v, name, &value, errp);
}

static void pci_virt_get_pci_hole64_end(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    PCIHostState *h = PCI_HOST_BRIDGE(obj);
    PCIVirtHost *s = PCI_VIRT_HOST(obj);
    uint64_t hole64_start = pci_virt_pci_hole64_start();
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

static void pci_virt_initfn(Object *obj)
{
    /* TODO: Can we even support legacy CFC/CF8 on non zero segments */
    PCIHostState *s = PCI_HOST_BRIDGE(obj);

    memory_region_init_io(&s->conf_mem, obj, &pci_host_conf_le_ops, s,
                          "pci-conf-idx", 4);
    memory_region_init_io(&s->data_mem, obj, &pci_host_data_le_ops, s,
                          "pci-conf-data", 4);

    object_property_add(obj, PCI_HOST_PROP_PCI_HOLE_START, "int",
                        pci_virt_get_pci_hole_start,
                        NULL, NULL, NULL, NULL);

    object_property_add(obj, PCI_HOST_PROP_PCI_HOLE_END, "int",
                        pci_virt_get_pci_hole_end,
                        NULL, NULL, NULL, NULL);

    object_property_add(obj, PCI_HOST_PROP_PCI_HOLE64_START, "int",
                        pci_virt_get_pci_hole64_start,
                        NULL, NULL, NULL, NULL);

    object_property_add(obj, PCI_HOST_PROP_PCI_HOLE64_END, "int",
                        pci_virt_get_pci_hole64_end,
                        NULL, NULL, NULL, NULL);

}

static void pci_virt_set_irq(void *opaque, int irq_num, int level)
{
    /* TODO MSI Only */
    PCIVirtHost *d = opaque;

    qemu_set_irq(d->irq[irq_num], level); 
}

static void pci_virt_realize(DeviceState *dev, Error **errp)
{
    /* TODO: We do not add any IO Ports are IRQs here */
}

PCIBus *pci_virt_init(MemoryRegion *address_space_mem,
                      MemoryRegion *address_space_io,
                      MemoryRegion *pci_address_space)
{
    DeviceState *dev;
    PCIHostState *pci;
    PCIExpressHost *pcie;
    PCIVirtHost *pci_virt;

    dev = qdev_create(NULL, TYPE_PCI_VIRT_HOST);
    pci = PCI_HOST_BRIDGE(dev);
    pcie = PCIE_HOST_BRIDGE(dev);

    pci->bus = pci_register_root_bus(dev, "1.pcie.0", pci_virt_set_irq,
                                pci_swizzle_map_irq_fn, pci, pci_address_space,
                                address_space_io, 0, 4, TYPE_PCIE_BUS); 

    //TODO: This is pretty static, but this is due to the way that
    //acpi tries to build the mcfg
    //acpi_get_mcfg(AcpiMcfgInfo *mcfg) ->
    //   pci_host = acpi_get_pci_host();
    //which looks at and assumes a single PCI hierarchy
    //static const char *pci_hosts[] = {
    //   "/machine/i440fx",
    //   "/machine/q35",
    //   "/machine/pcilite",
    //   "/machine/pcivirt",
    //    NULL,
    object_property_add_child(qdev_get_machine(), "pcivirt", OBJECT(dev), NULL);
    qdev_init_nofail(dev);

    pci_virt = PCI_VIRT_HOST(dev);
    range_set_bounds(&pci_virt->pci_hole,
                    PCI_VIRT_HOLE_START_BASE,
                    PCI_VIRT_HOLE_START_BASE + PCI_VIRT_PCIEXBAR_SIZE);

    
    range_set_bounds(&pci_virt->pci_hole64, PCI_VIRT_HOLE64_START_BASE,
		    PCI_VIRT_HOLE64_START_BASE+DEFAULT_PCI_HOLE64_SIZE);

    //No legacy IRQs and IO
    //pci_virt = PCI_VIRT_HOST(dev);

    pcie_host_mmcfg_update(pcie, 1, PCI_VIRT_PCIEXBAR_BASE,
                           PCI_VIRT_PCIEXBAR_SIZE);
    e820_add_entry(PCI_VIRT_PCIEXBAR_BASE, PCI_VIRT_PCIEXBAR_SIZE,
                   E820_RESERVED);

    /* setup pci memory mapping */
    pc_pci_as_mapping_init(OBJECT(dev), address_space_mem, pci_address_space);

    //Directly place the network device on the bus???
    //pci_create_simple(pci->bus, 0, TYPE_PCI_VIRT_DEVICE);
    //pci_create_simple(pci->bus, 0, TYPE_VIRTIO_NET_PCI);
    return pci->bus;
}

static const char *pci_virt_root_bus_path(PCIHostState *host_bridge,
                                          PCIBus *rootbus)
{
    return "0001:00";  //TODO: This needs to be dynamic based on the segment
}

static Property pci_virt_props[] = {
    DEFINE_PROP_UINT64(PCIE_HOST_MCFG_BASE, PCIVirtHost,
                       parent_obj.base_addr, PCI_VIRT_PCIEXBAR_BASE),
    DEFINE_PROP_UINT64(PCIE_HOST_MCFG_SIZE, PCIVirtHost,
                       parent_obj.size, PCI_VIRT_PCIEXBAR_SIZE),
    DEFINE_PROP_SIZE(PCI_HOST_PROP_PCI_HOLE64_SIZE, PCIVirtHost,
                     pci_hole64_size, DEFAULT_PCI_HOLE64_SIZE),
    DEFINE_PROP_END_OF_LIST(),
};

static void pci_virt_host_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories); /*TODO: We do not really want a bridge here
						      * we need to test without this set and
						      * check if enumeration works
						      */

    dc->realize = pci_virt_realize;
    dc->props = pci_virt_props;
    hc->root_bus_path = pci_virt_root_bus_path;
}

static const TypeInfo pci_virt_host_info = {
    .name          = TYPE_PCI_VIRT_HOST,
    .parent        = TYPE_PCIE_HOST_BRIDGE, //TODO: Can we use something more generic
    .instance_size = sizeof(PCIVirtHost),
    .instance_init = pci_virt_initfn,
    .class_init    = pci_virt_host_class_init,
};

static void pci_virt_device_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    //TODO: We do not want a bridge
    //Will linux register a bus without seeing a bridge
    k->class_id = PCI_CLASS_BRIDGE_HOST; 
    dc->desc = "Virt Host bridge"; 

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

static const TypeInfo pci_virt_device_info = {
    .name          = TYPE_PCI_VIRT_DEVICE,
    .parent        = TYPE_PCI_DEVICE,
    .class_init    = pci_virt_device_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void pci_virt_register_types(void)
{
    type_register_static(&pci_virt_device_info);
    type_register_static(&pci_virt_host_info);
}

type_init(pci_virt_register_types)
