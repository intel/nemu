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

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "contrib/libvhost-user/libvhost-user.h"

#define container_of(ptr, type, member) ({                              \
                        const typeof( ((type *)0)->member ) *__mptr = (ptr); \
                        (type *)( (char *)__mptr - offsetof(type,member) );})

struct fv_QueueInfo {
        pthread_t thread;
        struct fv_VuDev *virtio_dev;

        /* Our queue index, corresponds to array position */
        int       qidx;
        int       kick_fd;
};

/* We pass the dev element into libvhost-user
 * and then use it to get back to the outer
 * container for other data.
 */
struct fv_VuDev {
        VuDev dev;
        struct fuse_session *se;

        /* The following pair of fields are only accessed in the main
         * virtio_loop */
        size_t nqueues;
        struct fv_QueueInfo **qi;
};

/* From spec */
struct virtio_fs_config {
        char tag[36];
        uint32_t num_queues;
};

/* Callback from libvhost-user */
static uint64_t fv_get_features(VuDev *dev)
{
        return 1ULL << VIRTIO_F_VERSION_1;
}

/* Callback from libvhost-user */
static void fv_set_features(VuDev *dev, uint64_t features)
{
}

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

static void *fv_queue_thread(void *opaque)
{
        struct fv_QueueInfo *qi = opaque;
        fprintf(stderr, "%s: Start for queue %d kick_fd %d\n",
                __func__, qi->qidx, qi->kick_fd);
        while (1) {
                /* TODO */
        }

        return NULL;
}

/* Callback from libvhost-user on start or stop of a queue */
static void fv_queue_set_started(VuDev *dev, int qidx, bool started)
{
        struct fv_VuDev *vud = container_of(dev, struct fv_VuDev, dev);
        struct fv_QueueInfo *ourqi;

        fprintf(stderr, "%s: qidx=%d started=%d\n", __func__, qidx, started);
        assert(qidx>=0);

        if (qidx == 0) {
                /* This is a notification queue for us to tell the guest things
                 *  we don't expect
                 * any incoming from the guest here.
                 */
                return;
        }

        if (started) {
                /* Fire up a thread to watch this queue */
                if (qidx >= vud->nqueues) {
                        vud->qi = realloc(vud->qi, (qidx + 1) *
                                                   sizeof(vud->qi[0]));
                        assert(vud->qi);
                        memset(vud->qi + vud->nqueues, 0,
                               sizeof(vud->qi[0]) *
                                 (1 + (qidx - vud->nqueues)));
                        vud->nqueues = qidx + 1;
                }
                if (!vud->qi[qidx]) {
                        vud->qi[qidx] = calloc(sizeof(struct fv_QueueInfo), 1);
                        vud->qi[qidx]->virtio_dev = vud;
                        vud->qi[qidx]->qidx = qidx;
                        assert(vud->qi[qidx]);
                } else {
                        /* Shouldn't have been started */
                        assert(vud->qi[qidx]->kick_fd == -1);
                }
                ourqi = vud->qi[qidx];
                ourqi->kick_fd = dev->vq[qidx].kick_fd;
                if (pthread_create(&ourqi->thread, NULL,  fv_queue_thread,
                                   ourqi)) {
                        fprintf(stderr, "%s: Failed to create thread for queue %d\n",
                                __func__, qidx);
                        assert(0);
                }
        } else {
                /* TODO: Kill the thread */
                assert(qidx < vud->nqueues);
                ourqi = vud->qi[qidx];
                ourqi->kick_fd = -1;
        }
}

static bool fv_queue_order(VuDev *dev, int qidx)
{
        return false;
}

static const VuDevIface fv_iface = {
        .get_features = fv_get_features,
        .set_features = fv_set_features,

        /* Don't need process message, we've not got any at vhost-user level */
        .queue_set_started = fv_queue_set_started,

        .queue_is_processed_in_order = fv_queue_order,
};

/* Main loop; this mostly deals with events on the vhost-user
 * socket itself, and not actual fuse data.
 */
int virtio_loop(struct fuse_session *se)
{
       fprintf(stderr, "%s: Entry\n", __func__);

       while (!fuse_session_exited(se)) {
               struct pollfd pf[1];
               pf[0].fd = se->vu_socketfd;
               pf[0].events = POLLIN;
               pf[0].revents = 0;

               if (se->debug)
                       fprintf(stderr, "%s: Waiting for VU event\n", __func__);
               int poll_res = ppoll(pf, 1, NULL, NULL);

               if (poll_res == -1) {
                       if (errno == EINTR) {
                               fprintf(stderr, "%s: ppoll interrupted, going around\n", __func__);
                               continue;
                       }
                       perror("virtio_loop ppoll");
                       break;
               }
               assert(poll_res == 1);
               if (pf[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                       fprintf(stderr, "%s: Unexpected poll revents %x\n",
                                __func__, pf[0].revents);
                       break;
               }
               assert(pf[0].revents & POLLIN);
               if (se->debug)
                       fprintf(stderr, "%s: Got VU event\n", __func__);
               if (!vu_dispatch(&se->virtio_dev->dev)) {
                       fprintf(stderr, "%s: vu_dispatch failed\n", __func__);
                       break;
               }
       }

       fprintf(stderr, "%s: Exit\n", __func__);

       return 0;
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


