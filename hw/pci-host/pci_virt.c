/*
 * QEMU Light weight PCI Host Bridge Emulation
 *
 * Copyright (C) 2018 Intel Corporation.
 *
 * Author:
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
#include "hw/i386/pci.h"
#include "hw/i386/memory.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie_host.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci-host/pci-virt.h"
#include "hw/pci-host/pci-lite.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "sysemu/sysemu.h"
#include "hw/i386/ioapic.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "hw/i386/virt.h"
#include "hw/nvram/fw_cfg.h"

static uint64_t pci_virt_pci_mcfg_base(PCIExpressHost *pcie)
{
    uint64_t mcfg_base;
    VirtMachineState *vms = VIRT_MACHINE(qdev_get_machine());
    PCIHostState *h = vms->acpi_conf.pci_host[0];
    PCIExpressHost *e = PCIE_HOST_BRIDGE(h);
    PCIVirtHost *v = PCI_VIRT_HOST(pcie);

    mcfg_base = e->base_addr + e->size;
    mcfg_base += (v->segment_nr - 1) * PCI_VIRT_PCIEXBAR_SIZE;

    return mcfg_base;
}
/*
 * The 64bit pci hole starts after "above 4G RAM" and
 * potentially the space reserved for memory device.
 */
static uint64_t pci_virt_pci_hole64_start(PCIHostState *h)
{
    uint64_t hole64_start = 0;
    VirtMachineState *vms = VIRT_MACHINE(qdev_get_machine());
    PCILiteHost *pci_lite = PCI_LITE_HOST(vms->acpi_conf.pci_host[0]);
    PCIVirtHost *s = PCI_VIRT_HOST(h);

    hole64_start = range_upb(&pci_lite->pci_hole64) + 1;
    hole64_start += (s->segment_nr - 1) * DEFAULT_PCI_HOLE64_SIZE;
    return ROUND_UP(hole64_start, 1ULL << 30);
}

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
    PCIVirtHost *s = PCI_VIRT_HOST(obj);
    uint64_t value;

    value = pci_virt_pci_hole64_start(h);
    visit_type_uint64(v, name, &value, errp);
    range_set_bounds(&s->pci_hole64, value, range_upb(&s->pci_hole64));
}

static void pci_virt_get_pci_hole64_end(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    PCIHostState *h = PCI_HOST_BRIDGE(obj);
    PCIVirtHost *s = PCI_VIRT_HOST(obj);
    uint64_t hole64_start = pci_virt_pci_hole64_start(h);
    uint64_t hole64_end;

    hole64_end = ROUND_UP(hole64_start + s->pci_hole64_size, 1ULL << 30);
    visit_type_uint64(v, name, &hole64_end, errp);
    range_set_bounds(&s->pci_hole64, range_lob(&s->pci_hole64), hole64_end);
}

static void pci_virt_initfn(Object *obj)
{
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
}
static PCIHostState *pci_virt_init(DeviceState *dev,
                                   MemoryRegion *address_space_mem,
                                   MemoryRegion *address_space_io,
                                   MemoryRegion *pci_address_space)
{
    PCIHostState *pci;
    PCIExpressHost *pcie;
    PCIVirtHost *pci_virt;
    uint64_t mcfg_base, pci_hole_start, pci_hole_end;
    VirtMachineState *vms = VIRT_MACHINE(qdev_get_machine());

    pci = PCI_HOST_BRIDGE(dev);
    pcie = PCIE_HOST_BRIDGE(dev);

    pci->bus = pci_register_root_bus(dev, dev->id, pci_virt_set_irq,
                                pci_swizzle_map_irq_fn, pci, pci_address_space,
                                address_space_io, 0, 4, TYPE_PCIE_BUS); 
    pci_virt = PCI_VIRT_HOST(dev);

    mcfg_base = pci_virt_pci_mcfg_base(pcie);
    pci_hole_start = PCI_VIRT_HOLE_START_BASE +
                     (pci_virt->segment_nr - 1) * PCI_VIRT_PCIEXBAR_SIZE;
    pci_hole_end = pci_hole_start + PCI_VIRT_PCIEXBAR_SIZE;
    if (pci_hole_end < IO_APIC_DEFAULT_ADDRESS) {
        range_set_bounds(&pci_virt->pci_hole, pci_hole_start, pci_hole_end - 1);
    } else {
        fprintf(stderr, "pci-virt: no space for host bridge %d!\n",
                pci_virt->segment_nr);
        assert(0);
    }

    pcie_host_mmcfg_update(pcie, 1, mcfg_base, PCI_VIRT_PCIEXBAR_SIZE);

    e820_add_entry(mcfg_base, PCI_VIRT_PCIEXBAR_SIZE, E820_RESERVED);
    fw_cfg_modify_file(vms->acpi_conf.fw_cfg, "etc/e820", e820_table,
                       sizeof(struct e820_entry) * e820_entries);

    /* setup pci memory mapping */
    pc_pci_as_mapping_init(OBJECT(dev), address_space_mem, pci_address_space);
    return pci;
}

/* pci-virt host bridge realize */
static void pci_virt_realize(DeviceState *dev, Error **errp)
{
    MemoryRegion *pci_memory;
    PCIVirtHost *pci_virt = PCI_VIRT_HOST(dev);
    VirtMachineState *vms = VIRT_MACHINE(qdev_get_machine());
    uint16_t segment_nr = pci_virt->segment_nr;
    char name[15];

    vms->acpi_conf.total_segment++;
    pci_memory = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "pci_virt_%04x", segment_nr);
    memory_region_init(pci_memory, NULL, name, UINT64_MAX);


    vms->acpi_conf.pci_host = g_renew(PCIHostState*,
                                      vms->acpi_conf.pci_host,
                                      vms->acpi_conf.total_segment);
    vms->acpi_conf.pci_host[segment_nr] = pci_virt_init(dev,
                    get_system_memory(), get_system_io(), pci_memory);

    vms->pci_bus = g_renew(PCIBus*, vms->pci_bus, vms->acpi_conf.total_segment);
    vms->pci_bus[segment_nr] = vms->acpi_conf.pci_host[segment_nr]->bus;
}

static const char *pci_virt_root_bus_path(PCIHostState *host_bridge,
                                          PCIBus *rootbus)
{
    PCIVirtHost *pci_virt = PCI_VIRT_HOST(host_bridge);

    snprintf(pci_virt->bus_path, sizeof(pci_virt->bus_path),
             "%04x:00", pci_virt->segment_nr);
    return pci_virt->bus_path;
}

static Property pci_virt_props[] = {
    DEFINE_PROP_UINT16("segment-nr", PCIVirtHost, segment_nr, 1),
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

    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->props = pci_virt_props;
    hc->root_bus_path = pci_virt_root_bus_path;
    dc->realize = pci_virt_realize;
    dc->user_creatable = true;
}

static const TypeInfo pci_virt_host_info = {
    .name          = TYPE_PCI_VIRT_HOST,
    .parent        = TYPE_PCIE_HOST_BRIDGE,
    .instance_size = sizeof(PCIVirtHost),
    .instance_init = pci_virt_initfn,
    .class_init    = pci_virt_host_class_init,
};

static Property pci_virt_dev_props[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void pci_virt_device_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->desc = "Host bridge";

    // TODO: Use different one to GPEX?
    k->vendor_id = PCI_VENDOR_ID_REDHAT;
    k->device_id = PCI_DEVICE_ID_REDHAT_PCIE_HOST;
    k->revision = 0;

    dc->props = pci_virt_dev_props;
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
