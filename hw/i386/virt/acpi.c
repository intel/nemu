/*
 *
 * Copyright (c) 2018 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/range.h"
#include "qapi/error.h"
#include "qom/cpu.h"

#include "hw/hw.h"
#include "hw/hotplug.h"
#include "hw/sysbus.h"

#include "hw/i386/acpi.h"
#include "hw/i386/virt.h"

#include "hw/acpi/acpi.h"
#include "hw/acpi/cpu.h"
#include "hw/acpi/cpu_hotplug.h"
#include "hw/acpi/acpi_dev_interface.h"
#include "hw/acpi/memory_hotplug.h"
#include "hw/acpi/pc-hotplug.h"
#include "hw/acpi/reduced.h"
#include "hw/acpi/pcihp.h"
#include "hw/acpi/ged.h"

#include "hw/pci/pci.h"

typedef struct VirtAcpiState {
    SysBusDevice parent_obj;

    AcpiCpuHotplug cpuhp;
    CPUHotplugState cpuhp_state;

    MemHotplugState memhp_state;

    GEDState ged_state;

#if defined CONFIG_GED_IOAPIC
    qemu_irq *gsi;
#endif

    AcpiPciHpState pcihp_state;
    PCIBus *pci_bus;

    MemoryRegion sleep_iomem;
    MemoryRegion sleep_status_iomem;
    MemoryRegion reset_iomem;
} VirtAcpiState;

#define TYPE_VIRT_ACPI "virt-acpi"
#define VIRT_ACPI(obj) \
    OBJECT_CHECK(VirtAcpiState, (obj), TYPE_VIRT_ACPI)

static const VMStateDescription vmstate_acpi = {
    .name = "virt_acpi",
    .version_id = 1,
    .minimum_version_id = 1,
};

static void virt_device_plug_cb(HotplugHandler *hotplug_dev,
                                DeviceState *dev, Error **errp)
{
    VirtAcpiState *s = VIRT_ACPI(hotplug_dev);

    if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        acpi_cpu_plug_cb(hotplug_dev, &s->cpuhp_state, dev, errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
        if (object_dynamic_cast(OBJECT(dev), TYPE_NVDIMM)) {
            nvdimm_acpi_plug_cb(hotplug_dev, dev);
        } else {
            acpi_memory_plug_cb(hotplug_dev, &s->memhp_state,
                                dev, errp);
        }
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_PCI_DEVICE)) {
        acpi_pcihp_device_plug_cb(hotplug_dev, &s->pcihp_state, dev, errp);
    } else {
        error_setg(errp, "virt: device plug request for unsupported device"
                   " type: %s", object_get_typename(OBJECT(dev)));
    }
}

static void virt_device_unplug_request_cb(HotplugHandler *hotplug_dev,
                                          DeviceState *dev, Error **errp)
{
    VirtAcpiState *s = VIRT_ACPI(hotplug_dev);

    if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        acpi_cpu_unplug_request_cb(hotplug_dev, &s->cpuhp_state, dev, errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
        acpi_memory_unplug_request_cb(hotplug_dev, &s->memhp_state, dev, errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_PCI_DEVICE)) {
        acpi_pcihp_device_unplug_cb(hotplug_dev, &s->pcihp_state, dev, errp);
    }else {
        error_setg(errp, "virt: device unplug request for unsupported device"
                   " type: %s", object_get_typename(OBJECT(dev)));
    }
}

static void virt_device_unplug_cb(HotplugHandler *hotplug_dev,
                                  DeviceState *dev, Error **errp)
{
    VirtAcpiState *s = VIRT_ACPI(hotplug_dev);

    if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        acpi_cpu_unplug_cb(&s->cpuhp_state, dev, errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
        acpi_memory_unplug_cb(&s->memhp_state, dev, errp);
    } else {
        error_setg(errp, "virt: device unplug for unsupported device"
                   " type: %s", object_get_typename(OBJECT(dev)));
    }
}

static void virt_ospm_status(AcpiDeviceIf *adev, ACPIOSTInfoList ***list)
{
}

static void virt_send_ged(AcpiDeviceIf *adev, AcpiEventStatusBits ev)
{
    VirtAcpiState *s = VIRT_ACPI(adev);
    uint32_t sel = ACPI_GED_IRQ_SEL_INIT;

    if (ev & ACPI_CPU_HOTPLUG_STATUS) {
        sel = ACPI_GED_IRQ_SEL_CPU;
    } else if (ev & ACPI_MEMORY_HOTPLUG_STATUS) {
        sel = ACPI_GED_IRQ_SEL_MEM;
    } else if (ev & ACPI_NVDIMM_HOTPLUG_STATUS) {
        sel = ACPI_GED_IRQ_SEL_NVDIMM;
    } else if (ev & ACPI_PCI_HOTPLUG_STATUS) {
        sel = ACPI_GED_IRQ_SEL_PCI;
    } else {
        /* Unknown event. Return without generating interrupt. */
        return;
    }

    acpi_ged_set_event(&s->ged_state, sel);

#ifdef CONFIG_GED_IOAPIC
    /* We inject the hotplug interrupt by IOAPIC IRQ.
     * The IRQ selector will make the difference from the ACPI table.
     */
    acpi_ged_irq(&s->ged_state, s->gsi);
#endif
}

static int virt_device_sysbus_init(SysBusDevice *dev)
{
    return 0;
}

static void virt_acpi_sleep_cnt_write(void *opaque, hwaddr addr,
                                      uint64_t val, unsigned width)
{
    uint16_t sus_type = (val >> 2) & 7;

    if (val & ACPI_REDUCED_SLEEP_ENABLE) {
        switch (sus_type) {
        case ACPI_REDUCED_SLEEP_LEVEL:
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            break;
        default:
            break;
        }
    }
}

static const MemoryRegionOps virt_sleep_cnt_ops = {
    .write = virt_acpi_sleep_cnt_write,
};


// NOOP I/O port as the Linux kernel expects to write to the sleep status
// register on HW-reduced shutdown
static void virt_acpi_sleep_status_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned width)
{
    return;
}

static uint64_t virt_acpi_sleep_status_read(void *opaque, hwaddr addr,
                                            unsigned width)
{
    return 0;
}

const MemoryRegionOps virt_sleep_status_ops = {
        .write = virt_acpi_sleep_status_write,
        .read = virt_acpi_sleep_status_read,
};

static void virt_acpi_reset_write(void *opaque, hwaddr addr,
                                      uint64_t val, unsigned width)
{
    if (val & ACPI_REDUCED_RESET_VALUE) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return;
    }
}

static const MemoryRegionOps virt_reset_ops = {
    .write = virt_acpi_reset_write,
};

static void virt_device_realize(DeviceState *dev, Error **errp)
{
    VirtAcpiState *s = VIRT_ACPI(dev);
    SysBusDevice *sys = SYS_BUS_DEVICE(dev);
#ifdef CONFIG_GED_IOAPIC
    uint32_t ged_ioapic_irq = VIRT_ACPI_GED_IRQ;

    acpi_ged_init(get_system_io(), OBJECT(dev), &s->ged_state,
                  ACPI_GED_EVENT_IO_BASE, &ged_ioapic_irq);
#endif

    s->cpuhp.device = OBJECT(s);

    cpu_hotplug_hw_init(get_system_io(), s->cpuhp.device,
                        &s->cpuhp_state, VIRT_CPU_HOTPLUG_IO_BASE);

    acpi_memory_hotplug_init(get_system_io(), OBJECT(dev),
                             &s->memhp_state, ACPI_MEMORY_HOTPLUG_BASE);

    memory_region_init_io(&s->sleep_iomem, OBJECT(dev),
                          &virt_sleep_cnt_ops, s, TYPE_VIRT_ACPI, 1);
    sysbus_add_io(sys, ACPI_REDUCED_SLEEP_CONTROL_IOPORT, &s->sleep_iomem);

    memory_region_init_io(&s->sleep_status_iomem, OBJECT(dev),
                          &virt_sleep_status_ops, s, TYPE_VIRT_ACPI, 1);
    sysbus_add_io(sys, ACPI_REDUCED_SLEEP_STATUS_IOPORT, &s->sleep_status_iomem);

    memory_region_init_io(&s->reset_iomem, OBJECT(dev),
                          &virt_reset_ops, s, TYPE_VIRT_ACPI, 1);
    sysbus_add_io(sys, ACPI_REDUCED_RESET_IOPORT, &s->reset_iomem);
}

DeviceState *virt_acpi_init(PCIBus *pci_bus)
{
    DeviceState *dev;
    VirtAcpiState *s;

    dev = sysbus_create_simple(TYPE_VIRT_ACPI, -1, NULL);

    s = VIRT_ACPI(dev);
    s->pci_bus = pci_bus;

    if (pci_bus) {
        /* Initialize PCI hotplug */
        qbus_set_hotplug_handler(BUS(pci_bus), dev, NULL);

        acpi_pcihp_init(OBJECT(s), &s->pcihp_state, s->pci_bus,
                        get_system_io(), true, VIRT_ACPI_PCI_HOTPLUG_IO_BASE);
        acpi_pcihp_reset(&s->pcihp_state);
    }

    return dev;
}

#ifdef CONFIG_GED_IOAPIC
void virt_acpi_init_gsi(DeviceState *dev, qemu_irq *gsi)
{
    VirtAcpiState *s = VIRT_ACPI(dev);

    s->gsi = gsi;
}
#endif

static Property virt_acpi_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void virt_acpi_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    SysBusDeviceClass *sbc = SYS_BUS_DEVICE_CLASS(class);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(class);
    AcpiDeviceIfClass *adevc = ACPI_DEVICE_IF_CLASS(class);

    dc->desc = "ACPI";
    dc->vmsd = &vmstate_acpi;
    dc->props = virt_acpi_properties;
    dc->realize = virt_device_realize;

    sbc->init = virt_device_sysbus_init;

    hc->plug = virt_device_plug_cb;
    hc->unplug_request = virt_device_unplug_request_cb;
    hc->unplug = virt_device_unplug_cb;

    adevc->ospm_status = virt_ospm_status;
    adevc->send_event = virt_send_ged;
    adevc->madt_cpu = madt_cpu_entry;
}

static const TypeInfo virt_acpi_info = {
    .name          = TYPE_VIRT_ACPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VirtAcpiState),
    .class_init    = virt_acpi_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_HOTPLUG_HANDLER },
        { TYPE_ACPI_DEVICE_IF },
        { }
    }
};

static void virt_acpi_register_types(void)
{
    type_register_static(&virt_acpi_info);
}

type_init(virt_acpi_register_types)
