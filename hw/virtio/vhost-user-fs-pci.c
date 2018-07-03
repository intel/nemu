/*
 * Vhost-user filesystem virtio device PCI glue
 *
 * Copyright 2018-2019 Red Hat, Inc.
 *
 * Authors:
 *  Dr. David Alan Gilbert <dgilbert@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/virtio/vhost-user-fs.h"
#include "virtio-pci.h"
#include "standard-headers/linux/virtio_fs.h"

struct VHostUserFSPCI {
    VirtIOPCIProxy parent_obj;
    VHostUserFS vdev;
    MemoryRegion cachebar;
};

typedef struct VHostUserFSPCI VHostUserFSPCI;

#define TYPE_VHOST_USER_FS_PCI "vhost-user-fs-pci"

#define VHOST_USER_FS_PCI(obj) \
        OBJECT_CHECK(VHostUserFSPCI, (obj), TYPE_VHOST_USER_FS_PCI)

static Property vhost_user_fs_pci_properties[] = {
    /* TODO multiqueue */
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors, 4),
    DEFINE_PROP_END_OF_LIST(),
};

static void vhost_user_fs_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VHostUserFSPCI *dev = VHOST_USER_FS_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);
    uint64_t cachesize;

    qdev_set_parent_bus(vdev, BUS(&vpci_dev->bus));
    object_property_set_bool(OBJECT(vdev), true, "realized", errp);
    cachesize = memory_region_size(&dev->vdev.cache);

    /*
     * The bar starts with the data/DAX cache
     * Others will be added later.
     */
    memory_region_init(&dev->cachebar, OBJECT(vpci_dev),
                       "vhost-fs-pci-cachebar", cachesize);
    memory_region_add_subregion(&dev->cachebar, 0, &dev->vdev.cache);
    virtio_pci_add_shm_cap(vpci_dev, VIRTIO_FS_PCI_CACHE_BAR, 0, cachesize,
                           VIRTIO_FS_PCI_SHMCAP_ID_CACHE);

    /* After 'realized' so the memory region exists */
    pci_register_bar(&vpci_dev->pci_dev, VIRTIO_FS_PCI_CACHE_BAR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_PREFETCH |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &dev->cachebar);
}

static void vhost_user_fs_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);
    k->realize = vhost_user_fs_pci_realize;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    dc->props = vhost_user_fs_pci_properties;
    pcidev_k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    pcidev_k->device_id = PCI_DEVICE_ID_VIRTIO_FS;
    pcidev_k->revision = 0x00;
    pcidev_k->class_id = PCI_CLASS_STORAGE_OTHER;
}

static void vhost_user_fs_pci_instance_init(Object *obj)
{
    VHostUserFSPCI *dev = VHOST_USER_FS_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VHOST_USER_FS);
}

static const VirtioPCIDeviceTypeInfo vhost_user_fs_pci_info = {
    .generic_name  = TYPE_VHOST_USER_FS_PCI,
    .parent        = TYPE_VIRTIO_PCI,
    .instance_size = sizeof(VHostUserFSPCI),
    .instance_init = vhost_user_fs_pci_instance_init,
    .class_init    = vhost_user_fs_pci_class_init,
};

static void vhost_user_fs_pci_register(void)
{
    virtio_pci_types_register(&vhost_user_fs_pci_info);
}

type_init(vhost_user_fs_pci_register);
