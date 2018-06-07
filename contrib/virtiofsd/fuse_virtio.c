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
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

/* From spec */
struct virtio_fs_config {
        char tag[36];
        uint32_t num_queues;
};

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

        return -1;
}


