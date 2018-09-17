/*
 * Virtio PCI Bindings
 *
 * Copyright IBM, Corp. 2007
 * Copyright (c) 2009 CodeSourcery
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *  Paul Brook        <paul@codesourcery.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"

#include "standard-headers/linux/virtio_pci.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-input.h"
#include "hw/pci/pci.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/loader.h"
#include "sysemu/kvm.h"
#include "qemu/range.h"
#include "hw/virtio/virtio-bus.h"
#include "qapi/visitor.h"
#include "virtio-pci.h"


static Property virtio_input_pci_properties[] = {
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors, 2),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_input_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VirtIOInputPCI *vinput = VIRTIO_INPUT_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&vinput->vdev);

    qdev_set_parent_bus(vdev, BUS(&vpci_dev->bus));
    virtio_pci_force_virtio_1(vpci_dev);
    object_property_set_bool(OBJECT(vdev), true, "realized", errp);
}

static void virtio_input_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    dc->props = virtio_input_pci_properties;
    k->realize = virtio_input_pci_realize;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);

    pcidev_k->class_id = PCI_CLASS_INPUT_OTHER;
}

static void virtio_input_hid_kbd_pci_class_init(ObjectClass *klass, void *data)
{
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    pcidev_k->class_id = PCI_CLASS_INPUT_KEYBOARD;
}

static void virtio_input_hid_mouse_pci_class_init(ObjectClass *klass,
                                                  void *data)
{
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    pcidev_k->class_id = PCI_CLASS_INPUT_MOUSE;
}

static void virtio_keyboard_initfn(Object *obj)
{
    VirtIOInputHIDPCI *dev = VIRTIO_INPUT_HID_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_KEYBOARD);
}

static void virtio_mouse_initfn(Object *obj)
{
    VirtIOInputHIDPCI *dev = VIRTIO_INPUT_HID_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_MOUSE);
}

static void virtio_tablet_initfn(Object *obj)
{
    VirtIOInputHIDPCI *dev = VIRTIO_INPUT_HID_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_TABLET);
}

static const TypeInfo virtio_input_pci_info = {
    .name          = TYPE_VIRTIO_INPUT_PCI,
    .parent        = TYPE_VIRTIO_PCI,
    .instance_size = sizeof(VirtIOInputPCI),
    .class_init    = virtio_input_pci_class_init,
    .abstract      = true,
};

static const TypeInfo virtio_input_hid_pci_info = {
    .name          = TYPE_VIRTIO_INPUT_HID_PCI,
    .parent        = TYPE_VIRTIO_INPUT_PCI,
    .instance_size = sizeof(VirtIOInputHIDPCI),
    .abstract      = true,
};

static const TypeInfo virtio_keyboard_pci_info = {
    .name          = TYPE_VIRTIO_KEYBOARD_PCI,
    .parent        = TYPE_VIRTIO_INPUT_HID_PCI,
    .class_init    = virtio_input_hid_kbd_pci_class_init,
    .instance_size = sizeof(VirtIOInputHIDPCI),
    .instance_init = virtio_keyboard_initfn,
};

static const TypeInfo virtio_mouse_pci_info = {
    .name          = TYPE_VIRTIO_MOUSE_PCI,
    .parent        = TYPE_VIRTIO_INPUT_HID_PCI,
    .class_init    = virtio_input_hid_mouse_pci_class_init,
    .instance_size = sizeof(VirtIOInputHIDPCI),
    .instance_init = virtio_mouse_initfn,
};

static const TypeInfo virtio_tablet_pci_info = {
    .name          = TYPE_VIRTIO_TABLET_PCI,
    .parent        = TYPE_VIRTIO_INPUT_HID_PCI,
    .instance_size = sizeof(VirtIOInputHIDPCI),
    .instance_init = virtio_tablet_initfn,
};

#ifdef CONFIG_LINUX
static void virtio_host_initfn(Object *obj)
{
    VirtIOInputHostPCI *dev = VIRTIO_INPUT_HOST_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_INPUT_HOST);
}

static const TypeInfo virtio_host_pci_info = {
    .name          = TYPE_VIRTIO_INPUT_HOST_PCI,
    .parent        = TYPE_VIRTIO_INPUT_PCI,
    .instance_size = sizeof(VirtIOInputHostPCI),
    .instance_init = virtio_host_initfn,
};
#endif

static void virtio_pci_input_register_types(void)
{
    type_register_static(&virtio_input_pci_info);
    type_register_static(&virtio_input_hid_pci_info);
    type_register_static(&virtio_keyboard_pci_info);
    type_register_static(&virtio_mouse_pci_info);
    type_register_static(&virtio_tablet_pci_info);
#ifdef CONFIG_LINUX
    type_register_static(&virtio_host_pci_info);
#endif
}

type_init(virtio_pci_input_register_types)
