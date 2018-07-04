/*
 * Vhost-user filesystem virtio device
 *
 * Copyright 2018 Red Hat, Inc.
 *
 * Authors:
 *  Stefan Hajnoczi <stefanha@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

#ifndef _QEMU_VHOST_USER_FS_H
#define _QEMU_VHOST_USER_FS_H

#include "hw/virtio/virtio.h"
#include "hw/virtio/vhost.h"
#include "hw/virtio/vhost-user.h"
#include "chardev/char-fe.h"

#define TYPE_VHOST_USER_FS "vhost-user-fs-device"
#define VHOST_USER_FS(obj) \
        OBJECT_CHECK(VHostUserFS, (obj), TYPE_VHOST_USER_FS)

/* Structures carried over the slave channel back to QEMU */
#define VHOST_USER_FS_SLAVE_ENTRIES 8

/* For the flags field of VhostUserFSSlaveMsg */
#define VHOST_USER_FS_FLAG_MAP_R (1ull << 0)
#define VHOST_USER_FS_FLAG_MAP_W (1ull << 1)

typedef struct {
    /* Offsets within the file being mapped */
    uint64_t fd_offset[VHOST_USER_FS_SLAVE_ENTRIES];
    /* Offsets within the cache */
    uint64_t c_offset[VHOST_USER_FS_SLAVE_ENTRIES];
    /* Lengths of sections */
    uint64_t len[VHOST_USER_FS_SLAVE_ENTRIES];
    /* Flags, from VHOST_USER_FS_FLAG_* */
    uint64_t flags[VHOST_USER_FS_SLAVE_ENTRIES];
} VhostUserFSSlaveMsg;

typedef struct {
    CharBackend chardev;
    char *tag;
    uint16_t num_queues;
    uint16_t queue_size;
    char *vhostfd;
    size_t cache_size;
} VHostUserFSConf;

typedef struct {
    /*< private >*/
    VirtIODevice parent;
    VHostUserFSConf conf;
    struct vhost_virtqueue *vhost_vqs;
    struct vhost_dev vhost_dev;
    VhostUserState vhost_user;

    /*< public >*/
    MemoryRegion cache;
} VHostUserFS;

/* Callbacks from the vhost-user code for slave commands */
int vhost_user_fs_slave_map(struct vhost_dev *dev, VhostUserFSSlaveMsg *sm,
                            int fd);
int vhost_user_fs_slave_unmap(struct vhost_dev *dev, VhostUserFSSlaveMsg *sm);
int vhost_user_fs_slave_sync(struct vhost_dev *dev, VhostUserFSSlaveMsg *sm);

#endif /* _QEMU_VHOST_USER_FS_H */
