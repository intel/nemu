/*
  virtio-fs glue for FUSE
  Copyright (C) 2018 Red Hat, Inc. and/or its affiliates

  Authors:
    Dave Gilbert  <dgilbert@redhat.com>

  Implements the glue between libfuse and libvhost-user

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB
*/

#include "fuse_i.h"
#include "fuse_kernel.h"
#include "fuse_opt.h"
#include "fuse_misc.h"
#include "fuse_virtio.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "contrib/libvhost-user/libvhost-user.h"

/* We pass the dev element into libvhost-user
 * and then use it to get back to the outer
 * container for other data.
 */
struct fv_VuDev {
        VuDev dev;
        struct fuse_session *se;
};

/* From spec */
struct virtio_fs_config {
        char tag[36];
        uint32_t num_queues;
};

/* Callback from libvhost-user if there's a new fd we're supposed to listen
 * to, typically a queue kick?
 */
static void fv_set_watch(VuDev *dev, int fd, int condition, vu_watch_cb cb,
                         void *data)
{
        fprintf(stderr, "%s: TODO! fd=%d\n", __func__, fd);
}

/* Callback from libvhost-user if we're no longer supposed to listen on an fd
 */
static void fv_remove_watch(VuDev *dev, int fd)
{
        fprintf(stderr, "%s: TODO! fd=%d\n", __func__, fd);
}

/* Callback from libvhost-user to panic */
static void fv_panic(VuDev *dev, const char *err)
{
        fprintf(stderr, "%s: libvhost-user: %s\n", __func__, err);
        /* TODO: Allow reconnects?? */
        exit(EXIT_FAILURE);
}

static bool fv_queue_order(VuDev *dev, int qidx)
{
        return false;
}

static const VuDevIface fv_iface = {
        /* TODO: Add other callbacks */
        .queue_is_processed_in_order = fv_queue_order,
};

int virtio_loop(struct fuse_session *se)
{
       fprintf(stderr, "%s: Entry\n", __func__);

       while (1) {
           /* TODO: Add stuffing */
       }

       fprintf(stderr, "%s: Exit\n", __func__);
}

int virtio_session_mount(struct fuse_session *se)
{
        struct sockaddr_un un;

        if (strlen(se->vu_socket_path) >= sizeof(un.sun_path)) {
                fprintf(stderr, "Socket path too long\n");
                return -1;
        }

        /* Poison the fuse FD so we spot if we accidentally use it;
         * DO NOT check for this value, check for se->vu_socket_path
         */
        se->fd = 0xdaff0d11;

        /* Create the Unix socket to communicate with qemu
         * based on QEMU's vhost-user-bridge
         */
        unlink(se->vu_socket_path);
        strcpy(un.sun_path, se->vu_socket_path);
        size_t addr_len = sizeof(un.sun_family) + strlen(se->vu_socket_path);

        int listen_sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_sock == -1) {
               perror("vhost socket creation");
               return -1;
        }
        un.sun_family = AF_UNIX;

        if (bind(listen_sock, (struct sockaddr *) &un, addr_len) == -1) {
                perror("vhost socket bind");
                return -1;
        }

        if (listen(listen_sock, 1) == -1) {
                perror("vhost socket listen");
                return -1;
        }

        fprintf(stderr, "%s: Waiting for QEMU socket connection...\n", __func__);
        int data_sock = accept(listen_sock, NULL, NULL);
        if (data_sock == -1) {
                perror("vhost socket accept");
                close(listen_sock);
                return -1;
        }
        close(listen_sock);
        fprintf(stderr, "%s: Received QEMU socket connection\n", __func__);
        se->vu_socketfd = data_sock;

        /* TODO: Some cleanup/deallocation! */
        se->virtio_dev = calloc(sizeof(struct fv_VuDev), 1);
        se->virtio_dev->se = se;
        vu_init(&se->virtio_dev->dev, se->vu_socketfd,
                fv_panic,
                fv_set_watch, fv_remove_watch,
                &fv_iface);

        return 0;
}


