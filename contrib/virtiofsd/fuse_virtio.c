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
#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "contrib/libvhost-user/libvhost-user.h"

#define container_of(ptr, type, member) ({                              \
                        const typeof( ((type *)0)->member ) *__mptr = (ptr); \
                        (type *)( (char *)__mptr - offsetof(type,member) );})

struct fv_VuDev;
struct fv_QueueInfo {
        pthread_t thread;
        struct fv_VuDev *virtio_dev;

        /* Our queue index, corresponds to array position */
        int       qidx;
        int       kick_fd;
        int       kill_fd; /* For killing the thread */

        /* The element for the command currently being processed */
        VuVirtqElement *qe;
        bool      reply_sent;
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

/* Copy from an iovec into a fuse_buf (memory only)
 * Caller must ensure there is space
 */
static void copy_from_iov(struct fuse_buf *buf, size_t out_num,
                          const struct iovec *out_sg)
{
    void *dest = buf->mem;

    while (out_num) {
        size_t onelen = out_sg->iov_len;
        memcpy(dest, out_sg->iov_base, onelen);
        dest += onelen;
        out_sg++;
        out_num--;
    }
}

/* Copy from one iov to another, the given number of bytes
 * The caller must have checked sizes.
 */
static void copy_iov(struct iovec *src_iov, int src_count,
                     struct iovec *dst_iov, int dst_count,
                     size_t to_copy)
{
        size_t dst_offset = 0;
        /* Outer loop copies 'src' elements */
        while (to_copy) {
                assert(src_count);
                size_t src_len = src_iov[0].iov_len;
                size_t src_offset = 0;

                if (src_len > to_copy) {
                        src_len = to_copy;
                }
                /* Inner loop copies contents of one 'src' to maybe multiple
                 * dst.
                 */
                while (src_len) {
                        assert(dst_count);
                        size_t dst_len = dst_iov[0].iov_len - dst_offset;
                        if (dst_len > src_len) {
                                dst_len = src_len;
                        }

                        memcpy(dst_iov[0].iov_base + dst_offset,
                               src_iov[0].iov_base + src_offset,
                               dst_len);
                        src_len -= dst_len;
                        to_copy -= dst_len;
                        src_offset += dst_len;
                        dst_offset += dst_len;

                        assert(dst_offset <= dst_iov[0].iov_len);
                        if (dst_offset == dst_iov[0].iov_len) {
                                dst_offset = 0;
                                dst_iov++;
                                dst_count--;
                        }
                }
                src_iov++;
                src_count--;
        }
}

/* Called back by ll whenever it wants to send a reply/message back
 * The 1st element of the iov starts with the fuse_out_header
 * 'unique'==0 means it's a notify message.
 */
int virtio_send_msg(struct fuse_session *se, struct fuse_chan *ch,
                    struct iovec *iov, int count)
{
        VuVirtqElement *elem;
        VuVirtq *q;
        int ret = 0;

        assert(count >= 1);
        assert(iov[0].iov_len >= sizeof(struct fuse_out_header));

        struct fuse_out_header *out = iov[0].iov_base;
        // TODO: Endianness!

        size_t tosend_len = iov_length(iov, count);

        /* unique == 0 is notification, which we don't support */
        assert (out->unique);
        /* For virtio we always have ch */
        assert(ch);
        assert(!ch->qi->reply_sent);
        elem = ch->qi->qe;
        q = &ch->qi->virtio_dev->dev.vq[ch->qi->qidx];

        /* The 'in' part of the elem is to qemu */
        unsigned int in_num = elem->in_num;
        struct iovec *in_sg = elem->in_sg;
        size_t in_len = iov_length(in_sg, in_num);
        if (se->debug)
                fprintf(stderr, "%s: elem %d: with %d in desc of length %zd\n",
                        __func__, elem->index, in_num,  in_len);

        /* The elem should have room for a 'fuse_out_header' (out from fuse)
         * plus the data based on the len in the header.
         */
        if (in_len < sizeof(struct fuse_out_header)) {
                fprintf(stderr, "%s: elem %d too short for out_header\n",
                        __func__, elem->index);
                ret = -E2BIG;
                goto err;
        }
        if (in_len < tosend_len) {
                fprintf(stderr, "%s: elem %d too small for data len %zd\n",
                        __func__, elem->index, tosend_len);
                ret = -E2BIG;
                goto err;
        }

        copy_iov(iov, count, in_sg, in_num, tosend_len);
        vu_queue_push(&se->virtio_dev->dev, q, elem, tosend_len);
        vu_queue_notify(&se->virtio_dev->dev, q);
        ch->qi->reply_sent = true;

err:

        return ret;
}

/* Callback from fuse_send_data_iov_* when it's virtio and the buffer
 * is a single FD with FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK
 * We need send the iov and then the buffer.
 * Return 0 on success
 */
int virtio_send_data_iov(struct fuse_session *se, struct fuse_chan *ch,
                         struct iovec *iov, int count,
                         struct fuse_bufvec *buf, size_t len)
{
        int ret = 0;
        VuVirtqElement *elem;
        VuVirtq *q;

        assert(count >= 1);
        assert(iov[0].iov_len >= sizeof(struct fuse_out_header));

        struct fuse_out_header *out = iov[0].iov_base;
        // TODO: Endianness!

        size_t iov_len = iov_length(iov, count);
        size_t tosend_len = iov_len + len;

        out->len = tosend_len;

        if (se->debug)
                fprintf(stderr, "%s: count=%d len=%zd iov_len=%zd \n",
                        __func__, count, len, iov_len);

        /* unique == 0 is notification which we don't support */
        assert (out->unique);

        /* For virtio we always have ch */
        assert(ch);
        assert(!ch->qi->reply_sent);
        elem = ch->qi->qe;
        q = &ch->qi->virtio_dev->dev.vq[ch->qi->qidx];

        /* The 'in' part of the elem is to qemu */
        unsigned int in_num = elem->in_num;
        struct iovec *in_sg = elem->in_sg;
        size_t in_len = iov_length(in_sg, in_num);
        if (se->debug)
                fprintf(stderr, "%s: elem %d: with %d in desc of length %zd\n",
                        __func__, elem->index, in_num,  in_len);

        /* The elem should have room for a 'fuse_out_header' (out from fuse)
         * plus the data based on the len in the header.
         */
        if (in_len < sizeof(struct fuse_out_header)) {
                fprintf(stderr, "%s: elem %d too short for out_header\n",
                        __func__, elem->index);
                ret = -E2BIG;
                goto err;
        }
        if (in_len < tosend_len) {
                fprintf(stderr, "%s: elem %d too small for data len %zd\n",
                        __func__, elem->index, tosend_len);
                ret = -E2BIG;
                goto err;
        }

        // TODO: Limit to 'len'

        /* First copy the header data from iov->in_sg */
        copy_iov(iov, count, in_sg, in_num, iov_len);

        /* Build a copy of the the in_sg iov so we can skip bits in it,
         * including changing the offsets
         */
        struct iovec *in_sg_cpy = calloc(sizeof(struct iovec), in_num);
        memcpy(in_sg_cpy, in_sg, sizeof(struct iovec) * in_num);
        /* These get updated as we skip */
        struct iovec *in_sg_ptr = in_sg_cpy;
        int in_sg_cpy_count = in_num;

        /* skip over parts of in_sg that contained the header iov */
        size_t skip_size = iov_len;

        size_t in_sg_left = 0;
        do {
                while ( skip_size != 0 && in_sg_cpy_count) {
                        if (skip_size >= in_sg_ptr[0].iov_len) {
                                skip_size -= in_sg_ptr[0].iov_len;
                                in_sg_ptr++;
                                in_sg_cpy_count--;
                        } else {
                                in_sg_ptr[0].iov_len -= skip_size;
                                in_sg_ptr[0].iov_base += skip_size;
                                break;
                        }
                }

                int i;
                for (i = 0, in_sg_left = 0; i < in_sg_cpy_count; i++) {
                        in_sg_left += in_sg_ptr[i].iov_len;
                }
                if (se->debug)
                        fprintf(stderr,
                                "%s: after skip skip_size=%zd in_sg_cpy_count=%d in_sg_left=%zd\n",
                                __func__, skip_size, in_sg_cpy_count, in_sg_left);
                ret = preadv(buf->buf[0].fd, in_sg_ptr, in_sg_cpy_count, buf->buf[0].pos);

                if (se->debug)
                        fprintf(stderr, "%s: preadv_res=%d len=%zd\n",
                                __func__, ret, len);
                if (ret < len && ret) {
                        if (se->debug)
                                fprintf(stderr, "%s: ret < len\n", __func__);
                        /* Skip over this much next time around */
                        skip_size = ret;
                        buf->buf[0].pos += ret;
                        len -= ret;

                        /* Lets do another read */
                        continue;
                }
                if (!ret) {
                        /* EOF case? */
                        if (se->debug)
                                fprintf(stderr, "%s: !ret in_sg_left=%zd\n",
                                        __func__, in_sg_left);
                        break;
                }
                if (ret != len) {
                        if (se->debug)
                                fprintf(stderr, "%s: ret!=len\n", __func__);
                        ret = -EIO;
                        free(in_sg_cpy);
                        goto err;
                }
                in_sg_left -= ret;
                len -= ret;
        } while (in_sg_left);
        free(in_sg_cpy);

        // Need to fix out->len on EOF
        if (len) {
                struct fuse_out_header *out_sg = in_sg[0].iov_base;

                tosend_len -= len;
                out_sg->len = tosend_len;
        }

        ret = 0;

        vu_queue_push(&se->virtio_dev->dev, q, elem, tosend_len);
        vu_queue_notify(&se->virtio_dev->dev, q);

err:
        ch->qi->reply_sent = true;

        return ret;
}

/* Thread function for individual queues, created when a queue is 'started' */
static void *fv_queue_thread(void *opaque)
{
        struct fv_QueueInfo *qi = opaque;
        struct VuDev        *dev = &qi->virtio_dev->dev;
        struct VuVirtq      *q = vu_get_queue(dev, qi->qidx);
        struct fuse_session *se = qi->virtio_dev->se;
        struct fuse_chan    ch;
        struct fuse_buf     fbuf;

        fbuf.mem = NULL;
        fbuf.flags = 0;

        fuse_mutex_init(&ch.lock);
        ch.fd = (int)0xdaff0d111;
        ch.ctr = 1;
        ch.qi = qi;

        fprintf(stderr, "%s: Start for queue %d kick_fd %d\n",
                __func__, qi->qidx, qi->kick_fd);
        while (1) {
               struct pollfd pf[2];
               pf[0].fd = qi->kick_fd;
               pf[0].events = POLLIN;
               pf[0].revents = 0;
               pf[1].fd = qi->kill_fd;
               pf[1].events = POLLIN;
               pf[1].revents = 0;

               if (qi->virtio_dev->se->debug)
                       fprintf(stderr, "%s: Waiting for Queue %d event\n", __func__, qi->qidx);
               int poll_res = ppoll(pf, 2, NULL, NULL);

               if (poll_res == -1) {
                       if (errno == EINTR) {
                               fprintf(stderr, "%s: ppoll interrupted, going around\n", __func__);
                               continue;
                       }
                       perror("fv_queue_thread ppoll");
                       break;
               }
               assert(poll_res >= 1);
               if (pf[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                       fprintf(stderr, "%s: Unexpected poll revents %x Queue %d\n",
                                __func__, pf[0].revents, qi->qidx);
                       break;
               }
               if (pf[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                       fprintf(stderr, "%s: Unexpected poll revents %x Queue %d killfd\n",
                                __func__, pf[0].revents, qi->qidx);
                       break;
               }
               if (pf[1].revents) {
                       fprintf(stderr, "%s: kill event on queue %d - quitting\n",
                               __func__, qi->qidx);
                       break;
               }
               assert(pf[0].revents & POLLIN);
               if (qi->virtio_dev->se->debug)
                       fprintf(stderr, "%s: Got queue event on Queue %d\n", __func__, qi->qidx);

               eventfd_t evalue;
               if (eventfd_read(qi->kick_fd, &evalue)) {
                       perror("Eventfd_read for queue");
                       break;
               }
               /* out is from guest, in is too guest */
               unsigned int in_bytes, out_bytes;
               vu_queue_get_avail_bytes(dev, q, &in_bytes, &out_bytes, ~0, ~0);

               if (se->debug)
                       fprintf(stderr, "%s: Queue %d gave evalue: %zx available: in: %u out: %u\n",
                               __func__, qi->qidx, (size_t)evalue, in_bytes, out_bytes);

               if (!out_bytes) {
                       continue;
               }
               while (1) {
                       /* An element contains one request and the space to send our response
                        * They're spread over multiple descriptors in a scatter/gather set
                        * and we can't trust the guest to keep them still; so copy in/out.
                        */
                       VuVirtqElement *elem = vu_queue_pop(dev, q, sizeof(VuVirtqElement));
                       if (!elem) {
                               break;
                       }

                       qi->qe = elem;
                       qi->reply_sent = false;

                       if (!fbuf.mem) {
                               fbuf.mem = malloc(se->bufsize);
                               assert(fbuf.mem);
                               assert(se->bufsize > sizeof(struct fuse_in_header));
                       }
                       /* The 'out' part of the elem is from qemu */
                       unsigned int out_num = elem->out_num;
                       struct iovec *out_sg = elem->out_sg;
                       size_t out_len = iov_length(out_sg, out_num);
                       if (se->debug)
                               fprintf(stderr, "%s: elem %d: with %d out desc of length %zd\n",
                                       __func__, elem->index, out_num,  out_len);

                       /* The elem should contain a 'fuse_in_header' (in to fuse)
                        * plus the data based on the len in the header.
                        */
                       if (out_len < sizeof(struct fuse_in_header)) {
                               fprintf(stderr, "%s: elem %d too short for in_header\n",
                                       __func__, elem->index);
                               assert(0); // TODO
                       }
                       if (out_len > se->bufsize) {
                               fprintf(stderr, "%s: elem %d too large for buffer\n",
                                       __func__, elem->index);
                               assert(0); // TODO
                       }
                       copy_from_iov(&fbuf, out_num, out_sg);
                       fbuf.size = out_len;

                       // TODO! Endianness of header

                       // TODO: Add checks for fuse_session_exited
                       struct fuse_bufvec bufv = { .buf[0] = fbuf, .count = 1 };
                       fuse_session_process_buf_int(se, &bufv, &ch);

                       if (!qi->reply_sent) {
			       if (se->debug) {
				       fprintf(stderr,
					       "%s: elem %d no reply sent\n",
					       __func__, elem->index);
			       }
                               /* I think we've still got to recycle the element */
                               vu_queue_push(dev, q, elem, 0);
                               vu_queue_notify(dev, q);
                       }
                       qi->qe = NULL;
                       free(elem);
                       elem = NULL;
                }
        }
        pthread_mutex_destroy(&ch.lock);
        free(fbuf.mem);

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

                ourqi->kill_fd = eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
                assert(ourqi->kill_fd != -1);
                if (pthread_create(&ourqi->thread, NULL,  fv_queue_thread,
                                   ourqi)) {
                        fprintf(stderr, "%s: Failed to create thread for queue %d\n",
                                __func__, qidx);
                        assert(0);
                }
        } else {
                int ret;
                assert(qidx < vud->nqueues);
                ourqi = vud->qi[qidx];

                /* Kill the thread */
                if (eventfd_write(ourqi->kill_fd, 1)) {
                       perror("Eventfd_read for queue");
                }
                ret = pthread_join(ourqi->thread, NULL);
                if (ret) {
                       fprintf(stderr,
                               "%s: Failed to join thread idx %d err %d\n",
                               __func__, qidx, ret);
                }
                close(ourqi->kill_fd);
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

int fuse_virtio_map(fuse_req_t req, VhostUserFSSlaveMsg *msg, int fd)
{
        if (!req->se->virtio_dev) return -ENODEV;
        return !vu_fs_cache_request(&req->se->virtio_dev->dev,
                                    VHOST_USER_SLAVE_FS_MAP, fd, msg);
}

int fuse_virtio_unmap(struct fuse_session *se, VhostUserFSSlaveMsg *msg)
{
        if (!se->virtio_dev) return -ENODEV;
        return !vu_fs_cache_request(&se->virtio_dev->dev,
                                    VHOST_USER_SLAVE_FS_UNMAP, -1, msg);
}

int fuse_virtio_sync(fuse_req_t req, VhostUserFSSlaveMsg *msg)
{
        if (!req->se->virtio_dev) return -ENODEV;
        return !vu_fs_cache_request(&req->se->virtio_dev->dev,
                                    VHOST_USER_SLAVE_FS_SYNC, -1, msg);
}
