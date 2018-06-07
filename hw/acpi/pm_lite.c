/*
 * Light weight ACPI PM implementation
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright (C) 2016 Intel Corporation.
 *
 * Author:
 *  Chao Peng <chao.p.peng@linux.intel.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "hw/acpi/acpi.h"
#include "sysemu/sysemu.h"
#include "qapi/error.h"
#include "qemu/range.h"
#include "exec/ioport.h"
#include "hw/nvram/fw_cfg.h"
#include "exec/address-spaces.h"
#include "hw/acpi/pm_lite.h"
#include "hw/acpi/pcihp.h"
#include "hw/acpi/cpu_hotplug.h"
#include "hw/hotplug.h"
#include "hw/mem/pc-dimm.h"
#include "hw/acpi/memory_hotplug.h"
#include "hw/acpi/acpi_dev_interface.h"

#define PM_IO_BASE      0x600
#define GPE_BASE        0xafe0
#define GPE_LEN         4

typedef struct PMLiteState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    MemoryRegion io;
    MemoryRegion io_gpe;
    ACPIREGS ar;

    qemu_irq irq;
    Notifier machine_ready;
    Notifier powerdown_notifier;

    AcpiPciHpState acpi_pci_hotplug;
    bool use_acpi_pci_hotplug;

    uint8_t disable_s3;
    uint8_t disable_s4;
    uint8_t s4_val;
    bool cpu_hotplug_legacy;
    AcpiCpuHotplug gpe_cpu;
    CPUHotplugState cpuhp_state;

    MemHotplugState acpi_memory_hotplug;
} PMLiteState;

#define TYPE_PM_LITE "PM_LITE"

#define PM_LITE(obj) \
    OBJECT_CHECK(PMLiteState, (obj), TYPE_PM_LITE)

#define ACPI_ENABLE 0xf1
#define ACPI_DISABLE 0xf0

static void pm_tmr_timer(ACPIREGS *ar)
{
    PMLiteState *s = container_of(ar, PMLiteState, ar);
    acpi_update_sci(&s->ar, s->irq);
}

#define VMSTATE_GPE_ARRAY(_field, _state)                            \
 {                                                                   \
     .name       = (stringify(_field)),                              \
     .version_id = 0,                                                \
     .info       = &vmstate_info_uint16,                             \
     .size       = sizeof(uint16_t),                                 \
     .flags      = VMS_SINGLE | VMS_POINTER,                         \
     .offset     = vmstate_offset_pointer(_state, _field, uint8_t),  \
 }

static const VMStateDescription vmstate_gpe = {
    .name = "gpe",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_GPE_ARRAY(sts, ACPIGPE),
        VMSTATE_GPE_ARRAY(en, ACPIGPE),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_pci_status = {
    .name = "pci_status",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(up, struct AcpiPciHpPciStatus),
        VMSTATE_UINT32(down, struct AcpiPciHpPciStatus),
        VMSTATE_END_OF_LIST()
    }
};

#if 1
static bool vmstate_test_use_acpi_pci_hotplug(void *opaque, int version_id)
{
    PMLiteState *s = opaque;
    return s->use_acpi_pci_hotplug;
}
#endif
static bool vmstate_test_no_use_acpi_pci_hotplug(void *opaque, int version_id)
{
    PMLiteState *s = opaque;
    return !s->use_acpi_pci_hotplug;
}

static bool vmstate_test_use_memhp(void *opaque)
{
    PMLiteState *s = opaque;
    return s->acpi_memory_hotplug.is_enabled;
}

static const VMStateDescription vmstate_memhp_state = {
    .name = "pm_lite/memhp",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .needed = vmstate_test_use_memhp,
    .fields      = (VMStateField[]) {
        VMSTATE_MEMORY_HOTPLUG(acpi_memory_hotplug, PMLiteState),
        VMSTATE_END_OF_LIST()
    }
};

static bool vmstate_test_use_cpuhp(void *opaque)
{
    PMLiteState *s = opaque;
    return !s->cpu_hotplug_legacy;
}

static int vmstate_cpuhp_pre_load(void *opaque)
{
    Object *obj = OBJECT(opaque);
    object_property_set_bool(obj, false, "cpu-hotplug-legacy", &error_abort);
    return 0;
}

static const VMStateDescription vmstate_cpuhp_state = {
    .name = "pm_lite/cpuhp",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .needed = vmstate_test_use_cpuhp,
    .pre_load = vmstate_cpuhp_pre_load,
    .fields      = (VMStateField[]) {
        VMSTATE_CPU_HOTPLUG(cpuhp_state, PMLiteState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_acpi = {
    .name = "pm_lite",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, PMLiteState),
        VMSTATE_UINT16(ar.pm1.evt.sts, PMLiteState),
        VMSTATE_UINT16(ar.pm1.evt.en, PMLiteState),
        VMSTATE_UINT16(ar.pm1.cnt.cnt, PMLiteState),
        VMSTATE_TIMER_PTR(ar.tmr.timer, PMLiteState),
        VMSTATE_INT64(ar.tmr.overflow_time, PMLiteState),
        VMSTATE_STRUCT(ar.gpe, PMLiteState, 2, vmstate_gpe, ACPIGPE),
        VMSTATE_STRUCT_TEST(
            acpi_pci_hotplug.acpi_pcihp_pci_status[ACPI_PCIHP_BSEL_DEFAULT],
            PMLiteState,
            vmstate_test_no_use_acpi_pci_hotplug,
            2, vmstate_pci_status,
            struct AcpiPciHpPciStatus),
        VMSTATE_PCI_HOTPLUG(acpi_pci_hotplug, PMLiteState,
                            vmstate_test_use_acpi_pci_hotplug),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription*[]) {
         &vmstate_memhp_state,
         &vmstate_cpuhp_state,
         NULL
    }
};

static void pm_lite_reset(void *opaque)
{
    PMLiteState *s = opaque;
    acpi_pcihp_reset(&s->acpi_pci_hotplug);
}

static void pm_lite_powerdown_req(Notifier *n, void *opaque)
{
    PMLiteState *s = container_of(n, PMLiteState, powerdown_notifier);

    assert(s != NULL);
    acpi_pm1_evt_power_down(&s->ar);
}

static void pm_lite_device_plug_cb(HotplugHandler *hotplug_dev,
                                   DeviceState *dev, Error **errp)
{
    PMLiteState *s = PM_LITE(hotplug_dev);

    if (s->acpi_memory_hotplug.is_enabled &&
        object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
        acpi_memory_plug_cb(hotplug_dev, &s->acpi_memory_hotplug, dev, errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_PCI_DEVICE)) {
        acpi_pcihp_device_plug_cb(hotplug_dev, &s->acpi_pci_hotplug, dev, errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        if (s->cpu_hotplug_legacy) {
            legacy_acpi_cpu_plug_cb(hotplug_dev, &s->gpe_cpu, dev, errp);
        } else {
            acpi_cpu_plug_cb(hotplug_dev, &s->cpuhp_state, dev, errp);
        }
    } else {
        error_setg(errp, "acpi: device plug request for not supported device"
                   " type: %s", object_get_typename(OBJECT(dev)));
    }
}

static void pm_lite_device_unplug_request_cb(HotplugHandler *hotplug_dev,
                                             DeviceState *dev, Error **errp)
{
    PMLiteState *s = PM_LITE(hotplug_dev);

    if (s->acpi_memory_hotplug.is_enabled &&
        object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
        acpi_memory_unplug_request_cb(hotplug_dev, &s->acpi_memory_hotplug,
                                      dev, errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_PCI_DEVICE)) {
        acpi_pcihp_device_unplug_cb(hotplug_dev, &s->acpi_pci_hotplug, dev,
                                    errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_CPU) &&
               !s->cpu_hotplug_legacy) {
        acpi_cpu_unplug_request_cb(hotplug_dev, &s->cpuhp_state, dev, errp);
    } else {
        error_setg(errp, "acpi: device unplug request for not supported device"
                   " type: %s", object_get_typename(OBJECT(dev)));
    }
}

static void pm_lite_device_unplug_cb(HotplugHandler *hotplug_dev,
                                     DeviceState *dev, Error **errp)
{
    PMLiteState *s = PM_LITE(hotplug_dev);

    if (s->acpi_memory_hotplug.is_enabled &&
        object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
        acpi_memory_unplug_cb(&s->acpi_memory_hotplug, dev, errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_CPU) &&
               !s->cpu_hotplug_legacy) {
        acpi_cpu_unplug_cb(&s->cpuhp_state, dev, errp);
    } else {
        error_setg(errp, "acpi: device unplug for not supported device"
                   " type: %s", object_get_typename(OBJECT(dev)));
    }
}

static void pm_lite_update_bus_hotplug(PCIBus *pci_bus, void *opaque)
{
    PMLiteState *s = opaque;

    qbus_set_hotplug_handler(BUS(pci_bus), DEVICE(s), &error_abort);
}

static void pm_lite_machine_ready(Notifier *n, void *opaque)
{
    PMLiteState *s = container_of(n, PMLiteState, machine_ready);
    PCIDevice *d = PCI_DEVICE(s);

    if (s->use_acpi_pci_hotplug) {
        pci_for_each_bus(pci_get_bus(d), pm_lite_update_bus_hotplug, s);
    } else {
        pm_lite_update_bus_hotplug(pci_get_bus(d), s);
    }
}

static void pm_lite_add_propeties(PMLiteState *s)
{
    static const uint8_t acpi_enable_cmd = ACPI_ENABLE;
    static const uint8_t acpi_disable_cmd = ACPI_DISABLE;
    static const uint32_t pm_io_base = PM_IO_BASE;
    static const uint32_t gpe0_blk = GPE_BASE;
    static const uint32_t gpe0_blk_len = GPE_LEN;
    static const uint16_t sci_int = 9;

    object_property_add_uint8_ptr(OBJECT(s), ACPI_PM_PROP_ACPI_ENABLE_CMD,
                                  &acpi_enable_cmd, NULL);
    object_property_add_uint8_ptr(OBJECT(s), ACPI_PM_PROP_ACPI_DISABLE_CMD,
                                  &acpi_disable_cmd, NULL);
    object_property_add_uint32_ptr(OBJECT(s), ACPI_PM_PROP_PM_IO_BASE,
                                  &pm_io_base, NULL);
    object_property_add_uint32_ptr(OBJECT(s), ACPI_PM_PROP_GPE0_BLK,
                                  &gpe0_blk, NULL);
    object_property_add_uint32_ptr(OBJECT(s), ACPI_PM_PROP_GPE0_BLK_LEN,
                                  &gpe0_blk_len, NULL);
    object_property_add_uint16_ptr(OBJECT(s), ACPI_PM_PROP_SCI_INT,
                                  &sci_int, NULL);
}

Object *pm_lite_find(void)
{
    bool ambig;
    Object *o = object_resolve_path_type("", TYPE_PM_LITE, &ambig);

    if (ambig || !o) {
        return NULL;
    }
    return o;
}

DeviceState *pm_lite_init(PCIBus *bus, int devfn, qemu_irq sci_irq)
{
    DeviceState *dev;
    PMLiteState *s;

    dev = DEVICE(pci_create(bus, devfn, TYPE_PM_LITE));

    s = PM_LITE(dev);
    s->irq = sci_irq;

    qdev_init_nofail(dev);

    return dev;
}

static uint64_t gpe_readb(void *opaque, hwaddr addr, unsigned width)
{
    PMLiteState *s = opaque;
    uint32_t val = acpi_gpe_ioport_readb(&s->ar, addr);

    return val;
}

static void gpe_writeb(void *opaque, hwaddr addr, uint64_t val,
                       unsigned width)
{
    PMLiteState *s = opaque;

    acpi_gpe_ioport_writeb(&s->ar, addr, val);
    acpi_update_sci(&s->ar, s->irq);
}

static const MemoryRegionOps pm_lite_gpe_ops = {
    .read = gpe_readb,
    .write = gpe_writeb,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static bool pm_lite_get_cpu_hotplug_legacy(Object *obj, Error **errp)
{
    PMLiteState *s = PM_LITE(obj);

    return s->cpu_hotplug_legacy;
}

static void pm_lite_set_cpu_hotplug_legacy(Object *obj, bool value, Error **errp)
{
    PMLiteState *s = PM_LITE(obj);

    assert(!value);
    if (s->cpu_hotplug_legacy && value == false) {
        acpi_switch_to_modern_cphp(&s->gpe_cpu, &s->cpuhp_state,
                                   PM_LITE_CPU_HOTPLUG_IO_BASE);
    }
    s->cpu_hotplug_legacy = value;
}

static void pm_lite_acpi_system_hot_add_init(MemoryRegion *parent,
                                             PCIBus *bus, PMLiteState *s)
{
    memory_region_init_io(&s->io_gpe, OBJECT(s), &pm_lite_gpe_ops, s,
                          "acpi-gpe0", GPE_LEN);
    memory_region_add_subregion(parent, GPE_BASE, &s->io_gpe);

    acpi_pcihp_init(OBJECT(s), &s->acpi_pci_hotplug, bus, parent,
                    s->use_acpi_pci_hotplug);

    s->cpu_hotplug_legacy = true;
    object_property_add_bool(OBJECT(s), "cpu-hotplug-legacy",
                             pm_lite_get_cpu_hotplug_legacy,
                             pm_lite_set_cpu_hotplug_legacy,
                             NULL);

    legacy_acpi_cpu_hotplug_init(parent, OBJECT(s), &s->gpe_cpu,
                                 PM_LITE_CPU_HOTPLUG_IO_BASE);

    if (s->acpi_memory_hotplug.is_enabled) {
        acpi_memory_hotplug_init(parent, OBJECT(s), &s->acpi_memory_hotplug, ACPI_MEMORY_HOTPLUG_BASE);
    }
}

static void pm_lite_ospm_status(AcpiDeviceIf *adev, ACPIOSTInfoList ***list)
{
    PMLiteState *s = PM_LITE(adev);

    acpi_memory_ospm_status(&s->acpi_memory_hotplug, list);
    if (!s->cpu_hotplug_legacy) {
        acpi_cpu_ospm_status(&s->cpuhp_state, list);
    }
}

static void pm_lite_send_gpe(AcpiDeviceIf *adev, AcpiEventStatusBits ev)
{
    PMLiteState *s = PM_LITE(adev);

    acpi_send_gpe_event(&s->ar, s->irq, ev);
}

static Property pm_lite_properties[] = {
    DEFINE_PROP_UINT8(ACPI_PM_PROP_S3_DISABLED, PMLiteState, disable_s3, 0),
    DEFINE_PROP_UINT8(ACPI_PM_PROP_S4_DISABLED, PMLiteState, disable_s4, 0),
    DEFINE_PROP_UINT8(ACPI_PM_PROP_S4_VAL, PMLiteState, s4_val, 2),
    DEFINE_PROP_BOOL("acpi-pci-hotplug-with-bridge-support", PMLiteState,
                     use_acpi_pci_hotplug, true),
    DEFINE_PROP_BOOL("memory-hotplug-support", PMLiteState,
                     acpi_memory_hotplug.is_enabled, true),
    DEFINE_PROP_END_OF_LIST(),
};

static void pm_lite_realize(PCIDevice *dev, Error **errp)
{
    PMLiteState *s = PM_LITE(dev);

    memory_region_init(&s->io, OBJECT(s), "pm_lite", 64);
    memory_region_add_subregion(pci_address_space_io(dev), PM_IO_BASE, &s->io);

    acpi_pm_tmr_init(&s->ar, pm_tmr_timer, &s->io);
    acpi_pm1_evt_init(&s->ar, pm_tmr_timer, &s->io);
    acpi_pm1_cnt_init(&s->ar, &s->io, s->disable_s3, s->disable_s4, s->s4_val);
    acpi_gpe_init(&s->ar, GPE_LEN);

    s->powerdown_notifier.notify = pm_lite_powerdown_req;
    qemu_register_powerdown_notifier(&s->powerdown_notifier);

    s->machine_ready.notify = pm_lite_machine_ready;
    qemu_add_machine_init_done_notifier(&s->machine_ready);
    qemu_register_reset(pm_lite_reset, s);

    pm_lite_acpi_system_hot_add_init(pci_address_space_io(dev), pci_get_bus(dev), s);

    pm_lite_add_propeties(s);
}

static void pm_lite_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(klass);
    AcpiDeviceIfClass *adevc = ACPI_DEVICE_IF_CLASS(klass);

    k->realize = pm_lite_realize;
    k->class_id = PCI_CLASS_BRIDGE_OTHER;
    dc->desc = "PM LITE";
    dc->vmsd = &vmstate_acpi;
    dc->props = pm_lite_properties;
    /* Reason: part of pc-lite, needs to be wired up */
    dc->hotpluggable = false;
    hc->plug = pm_lite_device_plug_cb;
    hc->unplug_request = pm_lite_device_unplug_request_cb;
    hc->unplug = pm_lite_device_unplug_cb;
    adevc->ospm_status = pm_lite_ospm_status;
    adevc->send_event = pm_lite_send_gpe;
    adevc->madt_cpu = pc_madt_cpu_entry;
}

static const TypeInfo pm_lite_info = {
    .name          = TYPE_PM_LITE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PMLiteState),
    .class_init    = pm_lite_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_HOTPLUG_HANDLER },
        { TYPE_ACPI_DEVICE_IF },
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { }
    }
};

static void pm_lite_register_types(void)
{
    type_register_static(&pm_lite_info);
}

type_init(pm_lite_register_types)
