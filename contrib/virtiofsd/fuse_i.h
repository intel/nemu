/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB
*/

#ifndef FUSE_I_H
#define FUSE_I_H

#include "fuse.h"
#include "fuse_lowlevel.h"

struct fuse_req {
	struct fuse_session *se;
	uint64_t unique;
	int ctr;
	pthread_mutex_t lock;
	struct fuse_ctx ctx;
	struct fuse_chan *ch;
	int interrupted;
	unsigned int ioctl_64bit : 1;
	union {
		struct {
			uint64_t unique;
		} i;
		struct {
			fuse_interrupt_func_t func;
			void *data;
		} ni;
	} u;
	struct fuse_req *next;
	struct fuse_req *prev;
};

struct fuse_notify_req {
	uint64_t unique;
	void (*reply)(struct fuse_notify_req *, fuse_req_t, fuse_ino_t,
		      const void *, const struct fuse_buf *);
	struct fuse_notify_req *next;
	struct fuse_notify_req *prev;
};

struct fuse_session {
	char *mountpoint;
	volatile int exited;
	int fd;
	int debug;
	int deny_others;
	struct fuse_lowlevel_ops op;
	int got_init;
	struct cuse_data *cuse_data;
	void *userdata;
	uid_t owner;
	struct fuse_conn_info conn;
	struct fuse_req list;
	struct fuse_req interrupts;
	pthread_mutex_t lock;
	int got_destroy;
	int broken_splice_nonblock;
	uint64_t notify_ctr;
	struct fuse_notify_req notify_list;
	size_t bufsize;
	int error;
        char *vu_socket_path;
};

struct fuse_chan {
	pthread_mutex_t lock;
	int ctr;
	int fd;
};

/**
 * Filesystem module
 *
 * Filesystem modules are registered with the FUSE_REGISTER_MODULE()
 * macro.
 *
 */
struct fuse_module {
	char *name;
	fuse_module_factory_t factory;
	struct fuse_module *next;
	struct fusemod_so *so;
	int ctr;
};

/* ----------------------------------------------------------- *
 * Channel interface (when using -o clone_fd)		       *
 * ----------------------------------------------------------- */

/**
 * Obtain counted reference to the channel
 *
 * @param ch the channel
 * @return the channel
 */
struct fuse_chan *fuse_chan_get(struct fuse_chan *ch);

/**
 * Drop counted reference to a channel
 *
 * @param ch the channel
 */
void fuse_chan_put(struct fuse_chan *ch);

int fuse_send_reply_iov_nofree(fuse_req_t req, int error, struct iovec *iov,
			       int count);
void fuse_free_req(fuse_req_t req);

void fuse_session_process_buf_int(struct fuse_session *se,
				  const struct fuse_buf *buf, struct fuse_chan *ch);

#endif
