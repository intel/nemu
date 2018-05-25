/*
 * Block layer I/O functions
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "trace.h"
#include "sysemu/block-backend.h"
#include "block/aio-wait.h"
#include "block/blockjob.h"
#include "block/blockjob_int.h"
#include "block/block_int.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "qemu/error-report.h"

#define NOT_DONE 0x7fffffff /* used while emulated sync operation in progress */

/* Maximum bounce buffer for copy-on-read and write zeroes, in bytes */
#define MAX_BOUNCE_BUFFER (32768 << BDRV_SECTOR_BITS)

static int coroutine_fn bdrv_co_do_pwrite_zeroes(BlockDriverState *bs,
    int64_t offset, int bytes, BdrvRequestFlags flags);

void bdrv_parent_drained_begin(BlockDriverState *bs, BdrvChild *ignore)
{
    BdrvChild *c, *next;

    QLIST_FOREACH_SAFE(c, &bs->parents, next_parent, next) {
        if (c == ignore) {
            continue;
        }
        if (c->role->drained_begin) {
            c->role->drained_begin(c);
        }
    }
}

void bdrv_parent_drained_end(BlockDriverState *bs, BdrvChild *ignore)
{
    BdrvChild *c, *next;

    QLIST_FOREACH_SAFE(c, &bs->parents, next_parent, next) {
        if (c == ignore) {
            continue;
        }
        if (c->role->drained_end) {
            c->role->drained_end(c);
        }
    }
}

static void bdrv_merge_limits(BlockLimits *dst, const BlockLimits *src)
{
    dst->opt_transfer = MAX(dst->opt_transfer, src->opt_transfer);
    dst->max_transfer = MIN_NON_ZERO(dst->max_transfer, src->max_transfer);
    dst->opt_mem_alignment = MAX(dst->opt_mem_alignment,
                                 src->opt_mem_alignment);
    dst->min_mem_alignment = MAX(dst->min_mem_alignment,
                                 src->min_mem_alignment);
    dst->max_iov = MIN_NON_ZERO(dst->max_iov, src->max_iov);
}

void bdrv_refresh_limits(BlockDriverState *bs, Error **errp)
{
    BlockDriver *drv = bs->drv;
    Error *local_err = NULL;

    memset(&bs->bl, 0, sizeof(bs->bl));

    if (!drv) {
        return;
    }

    /* Default alignment based on whether driver has byte interface */
    bs->bl.request_alignment = drv->bdrv_co_preadv ? 1 : 512;

    /* Take some limits from the children as a default */
    if (bs->file) {
        bdrv_refresh_limits(bs->file->bs, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }
        bdrv_merge_limits(&bs->bl, &bs->file->bs->bl);
    } else {
        bs->bl.min_mem_alignment = 512;
        bs->bl.opt_mem_alignment = getpagesize();

        /* Safe default since most protocols use readv()/writev()/etc */
        bs->bl.max_iov = IOV_MAX;
    }

    if (bs->backing) {
        bdrv_refresh_limits(bs->backing->bs, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }
        bdrv_merge_limits(&bs->bl, &bs->backing->bs->bl);
    }

    /* Then let the driver override it */
    if (drv->bdrv_refresh_limits) {
        drv->bdrv_refresh_limits(bs, errp);
    }
}

/**
 * The copy-on-read flag is actually a reference count so multiple users may
 * use the feature without worrying about clobbering its previous state.
 * Copy-on-read stays enabled until all users have called to disable it.
 */
void bdrv_enable_copy_on_read(BlockDriverState *bs)
{
    atomic_inc(&bs->copy_on_read);
}

void bdrv_disable_copy_on_read(BlockDriverState *bs)
{
    int old = atomic_fetch_dec(&bs->copy_on_read);
    assert(old >= 1);
}

typedef struct {
    Coroutine *co;
    BlockDriverState *bs;
    bool done;
    bool begin;
    bool recursive;
    BdrvChild *parent;
} BdrvCoDrainData;

static void coroutine_fn bdrv_drain_invoke_entry(void *opaque)
{
    BdrvCoDrainData *data = opaque;
    BlockDriverState *bs = data->bs;

    if (data->begin) {
        bs->drv->bdrv_co_drain_begin(bs);
    } else {
        bs->drv->bdrv_co_drain_end(bs);
    }

    /* Set data->done before reading bs->wakeup.  */
    atomic_mb_set(&data->done, true);
    bdrv_wakeup(bs);
}

/* Recursively call BlockDriver.bdrv_co_drain_begin/end callbacks */
static void bdrv_drain_invoke(BlockDriverState *bs, bool begin, bool recursive)
{
    BdrvChild *child, *tmp;
    BdrvCoDrainData data = { .bs = bs, .done = false, .begin = begin};

    if (!bs->drv || (begin && !bs->drv->bdrv_co_drain_begin) ||
            (!begin && !bs->drv->bdrv_co_drain_end)) {
        return;
    }

    data.co = qemu_coroutine_create(bdrv_drain_invoke_entry, &data);
    bdrv_coroutine_enter(bs, data.co);
    BDRV_POLL_WHILE(bs, !data.done);

    if (recursive) {
        QLIST_FOREACH_SAFE(child, &bs->children, next, tmp) {
            bdrv_drain_invoke(child->bs, begin, true);
        }
    }
}

static bool bdrv_drain_recurse(BlockDriverState *bs)
{
    BdrvChild *child, *tmp;
    bool waited;

    /* Wait for drained requests to finish */
    waited = BDRV_POLL_WHILE(bs, atomic_read(&bs->in_flight) > 0);

    QLIST_FOREACH_SAFE(child, &bs->children, next, tmp) {
        BlockDriverState *bs = child->bs;
        bool in_main_loop =
            qemu_get_current_aio_context() == qemu_get_aio_context();
        assert(bs->refcnt > 0);
        if (in_main_loop) {
            /* In case the recursive bdrv_drain_recurse processes a
             * block_job_defer_to_main_loop BH and modifies the graph,
             * let's hold a reference to bs until we are done.
             *
             * IOThread doesn't have such a BH, and it is not safe to call
             * bdrv_unref without BQL, so skip doing it there.
             */
            bdrv_ref(bs);
        }
        waited |= bdrv_drain_recurse(bs);
        if (in_main_loop) {
            bdrv_unref(bs);
        }
    }

    return waited;
}

static void bdrv_do_drained_begin(BlockDriverState *bs, bool recursive,
                                  BdrvChild *parent);
static void bdrv_do_drained_end(BlockDriverState *bs, bool recursive,
                                BdrvChild *parent);

static void bdrv_co_drain_bh_cb(void *opaque)
{
    BdrvCoDrainData *data = opaque;
    Coroutine *co = data->co;
    BlockDriverState *bs = data->bs;

    bdrv_dec_in_flight(bs);
    if (data->begin) {
        bdrv_do_drained_begin(bs, data->recursive, data->parent);
    } else {
        bdrv_do_drained_end(bs, data->recursive, data->parent);
    }

    data->done = true;
    aio_co_wake(co);
}

static void coroutine_fn bdrv_co_yield_to_drain(BlockDriverState *bs,
                                                bool begin, bool recursive,
                                                BdrvChild *parent)
{
    BdrvCoDrainData data;

    /* Calling bdrv_drain() from a BH ensures the current coroutine yields and
     * other coroutines run if they were queued by aio_co_enter(). */

    assert(qemu_in_coroutine());
    data = (BdrvCoDrainData) {
        .co = qemu_coroutine_self(),
        .bs = bs,
        .done = false,
        .begin = begin,
        .recursive = recursive,
        .parent = parent,
    };
    bdrv_inc_in_flight(bs);
    aio_bh_schedule_oneshot(bdrv_get_aio_context(bs),
                            bdrv_co_drain_bh_cb, &data);

    qemu_coroutine_yield();
    /* If we are resumed from some other event (such as an aio completion or a
     * timer callback), it is a bug in the caller that should be fixed. */
    assert(data.done);
}

void bdrv_do_drained_begin(BlockDriverState *bs, bool recursive,
                           BdrvChild *parent)
{
    BdrvChild *child, *next;

    if (qemu_in_coroutine()) {
        bdrv_co_yield_to_drain(bs, true, recursive, parent);
        return;
    }

    /* Stop things in parent-to-child order */
    if (atomic_fetch_inc(&bs->quiesce_counter) == 0) {
        aio_disable_external(bdrv_get_aio_context(bs));
    }

    bdrv_parent_drained_begin(bs, parent);
    bdrv_drain_invoke(bs, true, false);
    bdrv_drain_recurse(bs);

    if (recursive) {
        bs->recursive_quiesce_counter++;
        QLIST_FOREACH_SAFE(child, &bs->children, next, next) {
            bdrv_do_drained_begin(child->bs, true, child);
        }
    }
}

void bdrv_drained_begin(BlockDriverState *bs)
{
    bdrv_do_drained_begin(bs, false, NULL);
}

void bdrv_subtree_drained_begin(BlockDriverState *bs)
{
    bdrv_do_drained_begin(bs, true, NULL);
}

void bdrv_do_drained_end(BlockDriverState *bs, bool recursive,
                         BdrvChild *parent)
{
    BdrvChild *child, *next;
    int old_quiesce_counter;

    if (qemu_in_coroutine()) {
        bdrv_co_yield_to_drain(bs, false, recursive, parent);
        return;
    }
    assert(bs->quiesce_counter > 0);
    old_quiesce_counter = atomic_fetch_dec(&bs->quiesce_counter);

    /* Re-enable things in child-to-parent order */
    bdrv_drain_invoke(bs, false, false);
    bdrv_parent_drained_end(bs, parent);
    if (old_quiesce_counter == 1) {
        aio_enable_external(bdrv_get_aio_context(bs));
    }

    if (recursive) {
        bs->recursive_quiesce_counter--;
        QLIST_FOREACH_SAFE(child, &bs->children, next, next) {
            bdrv_do_drained_end(child->bs, true, child);
        }
    }
}

void bdrv_drained_end(BlockDriverState *bs)
{
    bdrv_do_drained_end(bs, false, NULL);
}

void bdrv_subtree_drained_end(BlockDriverState *bs)
{
    bdrv_do_drained_end(bs, true, NULL);
}

void bdrv_apply_subtree_drain(BdrvChild *child, BlockDriverState *new_parent)
{
    int i;

    for (i = 0; i < new_parent->recursive_quiesce_counter; i++) {
        bdrv_do_drained_begin(child->bs, true, child);
    }
}

void bdrv_unapply_subtree_drain(BdrvChild *child, BlockDriverState *old_parent)
{
    int i;

    for (i = 0; i < old_parent->recursive_quiesce_counter; i++) {
        bdrv_do_drained_end(child->bs, true, child);
    }
}

void bdrv_drain(BlockDriverState *bs)
{
    bdrv_drained_begin(bs);
    bdrv_drained_end(bs);
}

/*
 * Wait for pending requests to complete across all BlockDriverStates
 *
 * This function does not flush data to disk, use bdrv_flush_all() for that
 * after calling this function.
 *
 * This pauses all block jobs and disables external clients. It must
 * be paired with bdrv_drain_all_end().
 *
 * NOTE: no new block jobs or BlockDriverStates can be created between
 * the bdrv_drain_all_begin() and bdrv_drain_all_end() calls.
 */
void bdrv_drain_all_begin(void)
{
    /* Always run first iteration so any pending completion BHs run */
    bool waited = true;
    BlockDriverState *bs;
    BdrvNextIterator it;
    GSList *aio_ctxs = NULL, *ctx;

    /* BDRV_POLL_WHILE() for a node can only be called from its own I/O thread
     * or the main loop AioContext. We potentially use BDRV_POLL_WHILE() on
     * nodes in several different AioContexts, so make sure we're in the main
     * context. */
    assert(qemu_get_current_aio_context() == qemu_get_aio_context());

    for (bs = bdrv_first(&it); bs; bs = bdrv_next(&it)) {
        AioContext *aio_context = bdrv_get_aio_context(bs);

        /* Stop things in parent-to-child order */
        aio_context_acquire(aio_context);
        aio_disable_external(aio_context);
        bdrv_parent_drained_begin(bs, NULL);
        bdrv_drain_invoke(bs, true, true);
        aio_context_release(aio_context);

        if (!g_slist_find(aio_ctxs, aio_context)) {
            aio_ctxs = g_slist_prepend(aio_ctxs, aio_context);
        }
    }

    /* Note that completion of an asynchronous I/O operation can trigger any
     * number of other I/O operations on other devices---for example a
     * coroutine can submit an I/O request to another device in response to
     * request completion.  Therefore we must keep looping until there was no
     * more activity rather than simply draining each device independently.
     */
    while (waited) {
        waited = false;

        for (ctx = aio_ctxs; ctx != NULL; ctx = ctx->next) {
            AioContext *aio_context = ctx->data;

            aio_context_acquire(aio_context);
            for (bs = bdrv_first(&it); bs; bs = bdrv_next(&it)) {
                if (aio_context == bdrv_get_aio_context(bs)) {
                    waited |= bdrv_drain_recurse(bs);
                }
            }
            aio_context_release(aio_context);
        }
    }

    g_slist_free(aio_ctxs);
}

void bdrv_drain_all_end(void)
{
    BlockDriverState *bs;
    BdrvNextIterator it;

    for (bs = bdrv_first(&it); bs; bs = bdrv_next(&it)) {
        AioContext *aio_context = bdrv_get_aio_context(bs);

        /* Re-enable things in child-to-parent order */
        aio_context_acquire(aio_context);
        bdrv_drain_invoke(bs, false, true);
        bdrv_parent_drained_end(bs, NULL);
        aio_enable_external(aio_context);
        aio_context_release(aio_context);
    }
}

void bdrv_drain_all(void)
{
    bdrv_drain_all_begin();
    bdrv_drain_all_end();
}

/**
 * Remove an active request from the tracked requests list
 *
 * This function should be called when a tracked request is completing.
 */
static void tracked_request_end(BdrvTrackedRequest *req)
{
    if (req->serialising) {
        atomic_dec(&req->bs->serialising_in_flight);
    }

    qemu_co_mutex_lock(&req->bs->reqs_lock);
    QLIST_REMOVE(req, list);
    qemu_co_queue_restart_all(&req->wait_queue);
    qemu_co_mutex_unlock(&req->bs->reqs_lock);
}

/**
 * Add an active request to the tracked requests list
 */
static void tracked_request_begin(BdrvTrackedRequest *req,
                                  BlockDriverState *bs,
                                  int64_t offset,
                                  unsigned int bytes,
                                  enum BdrvTrackedRequestType type)
{
    *req = (BdrvTrackedRequest){
        .bs = bs,
        .offset         = offset,
        .bytes          = bytes,
        .type           = type,
        .co             = qemu_coroutine_self(),
        .serialising    = false,
        .overlap_offset = offset,
        .overlap_bytes  = bytes,
    };

    qemu_co_queue_init(&req->wait_queue);

    qemu_co_mutex_lock(&bs->reqs_lock);
    QLIST_INSERT_HEAD(&bs->tracked_requests, req, list);
    qemu_co_mutex_unlock(&bs->reqs_lock);
}

static void mark_request_serialising(BdrvTrackedRequest *req, uint64_t align)
{
    int64_t overlap_offset = req->offset & ~(align - 1);
    unsigned int overlap_bytes = ROUND_UP(req->offset + req->bytes, align)
                               - overlap_offset;

    if (!req->serialising) {
        atomic_inc(&req->bs->serialising_in_flight);
        req->serialising = true;
    }

    req->overlap_offset = MIN(req->overlap_offset, overlap_offset);
    req->overlap_bytes = MAX(req->overlap_bytes, overlap_bytes);
}

/**
 * Round a region to cluster boundaries
 */
void bdrv_round_to_clusters(BlockDriverState *bs,
                            int64_t offset, int64_t bytes,
                            int64_t *cluster_offset,
                            int64_t *cluster_bytes)
{
    BlockDriverInfo bdi;

    if (bdrv_get_info(bs, &bdi) < 0 || bdi.cluster_size == 0) {
        *cluster_offset = offset;
        *cluster_bytes = bytes;
    } else {
        int64_t c = bdi.cluster_size;
        *cluster_offset = QEMU_ALIGN_DOWN(offset, c);
        *cluster_bytes = QEMU_ALIGN_UP(offset - *cluster_offset + bytes, c);
    }
}

static int bdrv_get_cluster_size(BlockDriverState *bs)
{
    BlockDriverInfo bdi;
    int ret;

    ret = bdrv_get_info(bs, &bdi);
    if (ret < 0 || bdi.cluster_size == 0) {
        return bs->bl.request_alignment;
    } else {
        return bdi.cluster_size;
    }
}

static bool tracked_request_overlaps(BdrvTrackedRequest *req,
                                     int64_t offset, unsigned int bytes)
{
    /*        aaaa   bbbb */
    if (offset >= req->overlap_offset + req->overlap_bytes) {
        return false;
    }
    /* bbbb   aaaa        */
    if (req->overlap_offset >= offset + bytes) {
        return false;
    }
    return true;
}

void bdrv_inc_in_flight(BlockDriverState *bs)
{
    atomic_inc(&bs->in_flight);
}

void bdrv_wakeup(BlockDriverState *bs)
{
    aio_wait_kick(bdrv_get_aio_wait(bs));
}

void bdrv_dec_in_flight(BlockDriverState *bs)
{
    atomic_dec(&bs->in_flight);
    bdrv_wakeup(bs);
}

static bool coroutine_fn wait_serialising_requests(BdrvTrackedRequest *self)
{
    BlockDriverState *bs = self->bs;
    BdrvTrackedRequest *req;
    bool retry;
    bool waited = false;

    if (!atomic_read(&bs->serialising_in_flight)) {
        return false;
    }

    do {
        retry = false;
        qemu_co_mutex_lock(&bs->reqs_lock);
        QLIST_FOREACH(req, &bs->tracked_requests, list) {
            if (req == self || (!req->serialising && !self->serialising)) {
                continue;
            }
            if (tracked_request_overlaps(req, self->overlap_offset,
                                         self->overlap_bytes))
            {
                /* Hitting this means there was a reentrant request, for
                 * example, a block driver issuing nested requests.  This must
                 * never happen since it means deadlock.
                 */
                assert(qemu_coroutine_self() != req->co);

                /* If the request is already (indirectly) waiting for us, or
                 * will wait for us as soon as it wakes up, then just go on
                 * (instead of producing a deadlock in the former case). */
                if (!req->waiting_for) {
                    self->waiting_for = req;
                    qemu_co_queue_wait(&req->wait_queue, &bs->reqs_lock);
                    self->waiting_for = NULL;
                    retry = true;
                    waited = true;
                    break;
                }
            }
        }
        qemu_co_mutex_unlock(&bs->reqs_lock);
    } while (retry);

    return waited;
}

static int bdrv_check_byte_request(BlockDriverState *bs, int64_t offset,
                                   size_t size)
{
    if (size > BDRV_REQUEST_MAX_SECTORS << BDRV_SECTOR_BITS) {
        return -EIO;
    }

    if (!bdrv_is_inserted(bs)) {
        return -ENOMEDIUM;
    }

    if (offset < 0) {
        return -EIO;
    }

    return 0;
}

typedef struct RwCo {
    BdrvChild *child;
    int64_t offset;
    QEMUIOVector *qiov;
    bool is_write;
    int ret;
    BdrvRequestFlags flags;
} RwCo;

static void coroutine_fn bdrv_rw_co_entry(void *opaque)
{
    RwCo *rwco = opaque;

    if (!rwco->is_write) {
        rwco->ret = bdrv_co_preadv(rwco->child, rwco->offset,
                                   rwco->qiov->size, rwco->qiov,
                                   rwco->flags);
    } else {
        rwco->ret = bdrv_co_pwritev(rwco->child, rwco->offset,
                                    rwco->qiov->size, rwco->qiov,
                                    rwco->flags);
    }
}

/*
 * Process a vectored synchronous request using coroutines
 */
static int bdrv_prwv_co(BdrvChild *child, int64_t offset,
                        QEMUIOVector *qiov, bool is_write,
                        BdrvRequestFlags flags)
{
    Coroutine *co;
    RwCo rwco = {
        .child = child,
        .offset = offset,
        .qiov = qiov,
        .is_write = is_write,
        .ret = NOT_DONE,
        .flags = flags,
    };

    if (qemu_in_coroutine()) {
        /* Fast-path if already in coroutine context */
        bdrv_rw_co_entry(&rwco);
    } else {
        co = qemu_coroutine_create(bdrv_rw_co_entry, &rwco);
        bdrv_coroutine_enter(child->bs, co);
        BDRV_POLL_WHILE(child->bs, rwco.ret == NOT_DONE);
    }
    return rwco.ret;
}

/*
 * Process a synchronous request using coroutines
 */
static int bdrv_rw_co(BdrvChild *child, int64_t sector_num, uint8_t *buf,
                      int nb_sectors, bool is_write, BdrvRequestFlags flags)
{
    QEMUIOVector qiov;
    struct iovec iov = {
        .iov_base = (void *)buf,
        .iov_len = nb_sectors * BDRV_SECTOR_SIZE,
    };

    if (nb_sectors < 0 || nb_sectors > BDRV_REQUEST_MAX_SECTORS) {
        return -EINVAL;
    }

    qemu_iovec_init_external(&qiov, &iov, 1);
    return bdrv_prwv_co(child, sector_num << BDRV_SECTOR_BITS,
                        &qiov, is_write, flags);
}

/* return < 0 if error. See bdrv_write() for the return codes */
int bdrv_read(BdrvChild *child, int64_t sector_num,
              uint8_t *buf, int nb_sectors)
{
    return bdrv_rw_co(child, sector_num, buf, nb_sectors, false, 0);
}

/* Return < 0 if error. Important errors are:
  -EIO         generic I/O error (may happen for all errors)
  -ENOMEDIUM   No media inserted.
  -EINVAL      Invalid sector number or nb_sectors
  -EACCES      Trying to write a read-only device
*/
int bdrv_write(BdrvChild *child, int64_t sector_num,
               const uint8_t *buf, int nb_sectors)
{
    return bdrv_rw_co(child, sector_num, (uint8_t *)buf, nb_sectors, true, 0);
}

int bdrv_pwrite_zeroes(BdrvChild *child, int64_t offset,
                       int bytes, BdrvRequestFlags flags)
{
    QEMUIOVector qiov;
    struct iovec iov = {
        .iov_base = NULL,
        .iov_len = bytes,
    };

    qemu_iovec_init_external(&qiov, &iov, 1);
    return bdrv_prwv_co(child, offset, &qiov, true,
                        BDRV_REQ_ZERO_WRITE | flags);
}

int bdrv_preadv(BdrvChild *child, int64_t offset, QEMUIOVector *qiov)
{
    int ret;

    ret = bdrv_prwv_co(child, offset, qiov, false, 0);
    if (ret < 0) {
        return ret;
    }

    return qiov->size;
}

int bdrv_pread(BdrvChild *child, int64_t offset, void *buf, int bytes)
{
    QEMUIOVector qiov;
    struct iovec iov = {
        .iov_base = (void *)buf,
        .iov_len = bytes,
    };

    if (bytes < 0) {
        return -EINVAL;
    }

    qemu_iovec_init_external(&qiov, &iov, 1);
    return bdrv_preadv(child, offset, &qiov);
}

int bdrv_pwritev(BdrvChild *child, int64_t offset, QEMUIOVector *qiov)
{
    int ret;

    ret = bdrv_prwv_co(child, offset, qiov, true, 0);
    if (ret < 0) {
        return ret;
    }

    return qiov->size;
}

int bdrv_pwrite(BdrvChild *child, int64_t offset, const void *buf, int bytes)
{
    QEMUIOVector qiov;
    struct iovec iov = {
        .iov_base   = (void *) buf,
        .iov_len    = bytes,
    };

    if (bytes < 0) {
        return -EINVAL;
    }

    qemu_iovec_init_external(&qiov, &iov, 1);
    return bdrv_pwritev(child, offset, &qiov);
}

/*
 * Writes to the file and ensures that no writes are reordered across this
 * request (acts as a barrier)
 *
 * Returns 0 on success, -errno in error cases.
 */
int bdrv_pwrite_sync(BdrvChild *child, int64_t offset,
                     const void *buf, int count)
{
    int ret;

    ret = bdrv_pwrite(child, offset, buf, count);
    if (ret < 0) {
        return ret;
    }

    ret = bdrv_flush(child->bs);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

typedef struct CoroutineIOCompletion {
    Coroutine *coroutine;
    int ret;
} CoroutineIOCompletion;

static void bdrv_co_io_em_complete(void *opaque, int ret)
{
    CoroutineIOCompletion *co = opaque;

    co->ret = ret;
    aio_co_wake(co->coroutine);
}

static int coroutine_fn bdrv_driver_preadv(BlockDriverState *bs,
                                           uint64_t offset, uint64_t bytes,
                                           QEMUIOVector *qiov, int flags)
{
    BlockDriver *drv = bs->drv;
    int64_t sector_num;
    unsigned int nb_sectors;

    assert(!(flags & ~BDRV_REQ_MASK));

    if (!drv) {
        return -ENOMEDIUM;
    }

    if (drv->bdrv_co_preadv) {
        return drv->bdrv_co_preadv(bs, offset, bytes, qiov, flags);
    }

    sector_num = offset >> BDRV_SECTOR_BITS;
    nb_sectors = bytes >> BDRV_SECTOR_BITS;

    assert((offset & (BDRV_SECTOR_SIZE - 1)) == 0);
    assert((bytes & (BDRV_SECTOR_SIZE - 1)) == 0);
    assert((bytes >> BDRV_SECTOR_BITS) <= BDRV_REQUEST_MAX_SECTORS);

    if (drv->bdrv_co_readv) {
        return drv->bdrv_co_readv(bs, sector_num, nb_sectors, qiov);
    } else {
        BlockAIOCB *acb;
        CoroutineIOCompletion co = {
            .coroutine = qemu_coroutine_self(),
        };

        acb = bs->drv->bdrv_aio_readv(bs, sector_num, qiov, nb_sectors,
                                      bdrv_co_io_em_complete, &co);
        if (acb == NULL) {
            return -EIO;
        } else {
            qemu_coroutine_yield();
            return co.ret;
        }
    }
}

static int coroutine_fn bdrv_driver_pwritev(BlockDriverState *bs,
                                            uint64_t offset, uint64_t bytes,
                                            QEMUIOVector *qiov, int flags)
{
    BlockDriver *drv = bs->drv;
    int64_t sector_num;
    unsigned int nb_sectors;
    int ret;

    assert(!(flags & ~BDRV_REQ_MASK));

    if (!drv) {
        return -ENOMEDIUM;
    }

    if (drv->bdrv_co_pwritev) {
        ret = drv->bdrv_co_pwritev(bs, offset, bytes, qiov,
                                   flags & bs->supported_write_flags);
        flags &= ~bs->supported_write_flags;
        goto emulate_flags;
    }

    sector_num = offset >> BDRV_SECTOR_BITS;
    nb_sectors = bytes >> BDRV_SECTOR_BITS;

    assert((offset & (BDRV_SECTOR_SIZE - 1)) == 0);
    assert((bytes & (BDRV_SECTOR_SIZE - 1)) == 0);
    assert((bytes >> BDRV_SECTOR_BITS) <= BDRV_REQUEST_MAX_SECTORS);

    if (drv->bdrv_co_writev_flags) {
        ret = drv->bdrv_co_writev_flags(bs, sector_num, nb_sectors, qiov,
                                        flags & bs->supported_write_flags);
        flags &= ~bs->supported_write_flags;
    } else if (drv->bdrv_co_writev) {
        assert(!bs->supported_write_flags);
        ret = drv->bdrv_co_writev(bs, sector_num, nb_sectors, qiov);
    } else {
        BlockAIOCB *acb;
        CoroutineIOCompletion co = {
            .coroutine = qemu_coroutine_self(),
        };

        acb = bs->drv->bdrv_aio_writev(bs, sector_num, qiov, nb_sectors,
                                       bdrv_co_io_em_complete, &co);
        if (acb == NULL) {
            ret = -EIO;
        } else {
            qemu_coroutine_yield();
            ret = co.ret;
        }
    }

emulate_flags:
    if (ret == 0 && (flags & BDRV_REQ_FUA)) {
        ret = bdrv_co_flush(bs);
    }

    return ret;
}

static int coroutine_fn
bdrv_driver_pwritev_compressed(BlockDriverState *bs, uint64_t offset,
                               uint64_t bytes, QEMUIOVector *qiov)
{
    BlockDriver *drv = bs->drv;

    if (!drv) {
        return -ENOMEDIUM;
    }

    if (!drv->bdrv_co_pwritev_compressed) {
        return -ENOTSUP;
    }

    return drv->bdrv_co_pwritev_compressed(bs, offset, bytes, qiov);
}

static int coroutine_fn bdrv_co_do_copy_on_readv(BdrvChild *child,
        int64_t offset, unsigned int bytes, QEMUIOVector *qiov)
{
    BlockDriverState *bs = child->bs;

    /* Perform I/O through a temporary buffer so that users who scribble over
     * their read buffer while the operation is in progress do not end up
     * modifying the image file.  This is critical for zero-copy guest I/O
     * where anything might happen inside guest memory.
     */
    void *bounce_buffer;

    BlockDriver *drv = bs->drv;
    struct iovec iov;
    QEMUIOVector local_qiov;
    int64_t cluster_offset;
    int64_t cluster_bytes;
    size_t skip_bytes;
    int ret;
    int max_transfer = MIN_NON_ZERO(bs->bl.max_transfer,
                                    BDRV_REQUEST_MAX_BYTES);
    unsigned int progress = 0;

    if (!drv) {
        return -ENOMEDIUM;
    }

    /* FIXME We cannot require callers to have write permissions when all they
     * are doing is a read request. If we did things right, write permissions
     * would be obtained anyway, but internally by the copy-on-read code. As
     * long as it is implemented here rather than in a separate filter driver,
     * the copy-on-read code doesn't have its own BdrvChild, however, for which
     * it could request permissions. Therefore we have to bypass the permission
     * system for the moment. */
    // assert(child->perm & (BLK_PERM_WRITE_UNCHANGED | BLK_PERM_WRITE));

    /* Cover entire cluster so no additional backing file I/O is required when
     * allocating cluster in the image file.  Note that this value may exceed
     * BDRV_REQUEST_MAX_BYTES (even when the original read did not), which
     * is one reason we loop rather than doing it all at once.
     */
    bdrv_round_to_clusters(bs, offset, bytes, &cluster_offset, &cluster_bytes);
    skip_bytes = offset - cluster_offset;

    trace_bdrv_co_do_copy_on_readv(bs, offset, bytes,
                                   cluster_offset, cluster_bytes);

    bounce_buffer = qemu_try_blockalign(bs,
                                        MIN(MIN(max_transfer, cluster_bytes),
                                            MAX_BOUNCE_BUFFER));
    if (bounce_buffer == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    while (cluster_bytes) {
        int64_t pnum;

        ret = bdrv_is_allocated(bs, cluster_offset,
                                MIN(cluster_bytes, max_transfer), &pnum);
        if (ret < 0) {
            /* Safe to treat errors in querying allocation as if
             * unallocated; we'll probably fail again soon on the
             * read, but at least that will set a decent errno.
             */
            pnum = MIN(cluster_bytes, max_transfer);
        }

        assert(skip_bytes < pnum);

        if (ret <= 0) {
            /* Must copy-on-read; use the bounce buffer */
            iov.iov_base = bounce_buffer;
            iov.iov_len = pnum = MIN(pnum, MAX_BOUNCE_BUFFER);
            qemu_iovec_init_external(&local_qiov, &iov, 1);

            ret = bdrv_driver_preadv(bs, cluster_offset, pnum,
                                     &local_qiov, 0);
            if (ret < 0) {
                goto err;
            }

            bdrv_debug_event(bs, BLKDBG_COR_WRITE);
            if (drv->bdrv_co_pwrite_zeroes &&
                buffer_is_zero(bounce_buffer, pnum)) {
                /* FIXME: Should we (perhaps conditionally) be setting
                 * BDRV_REQ_MAY_UNMAP, if it will allow for a sparser copy
                 * that still correctly reads as zero? */
                ret = bdrv_co_do_pwrite_zeroes(bs, cluster_offset, pnum, 0);
            } else {
                /* This does not change the data on the disk, it is not
                 * necessary to flush even in cache=writethrough mode.
                 */
                ret = bdrv_driver_pwritev(bs, cluster_offset, pnum,
                                          &local_qiov, 0);
            }

            if (ret < 0) {
                /* It might be okay to ignore write errors for guest
                 * requests.  If this is a deliberate copy-on-read
                 * then we don't want to ignore the error.  Simply
                 * report it in all cases.
                 */
                goto err;
            }

            qemu_iovec_from_buf(qiov, progress, bounce_buffer + skip_bytes,
                                pnum - skip_bytes);
        } else {
            /* Read directly into the destination */
            qemu_iovec_init(&local_qiov, qiov->niov);
            qemu_iovec_concat(&local_qiov, qiov, progress, pnum - skip_bytes);
            ret = bdrv_driver_preadv(bs, offset + progress, local_qiov.size,
                                     &local_qiov, 0);
            qemu_iovec_destroy(&local_qiov);
            if (ret < 0) {
                goto err;
            }
        }

        cluster_offset += pnum;
        cluster_bytes -= pnum;
        progress += pnum - skip_bytes;
        skip_bytes = 0;
    }
    ret = 0;

err:
    qemu_vfree(bounce_buffer);
    return ret;
}

/*
 * Forwards an already correctly aligned request to the BlockDriver. This
 * handles copy on read, zeroing after EOF, and fragmentation of large
 * reads; any other features must be implemented by the caller.
 */
static int coroutine_fn bdrv_aligned_preadv(BdrvChild *child,
    BdrvTrackedRequest *req, int64_t offset, unsigned int bytes,
    int64_t align, QEMUIOVector *qiov, int flags)
{
    BlockDriverState *bs = child->bs;
    int64_t total_bytes, max_bytes;
    int ret = 0;
    uint64_t bytes_remaining = bytes;
    int max_transfer;

    assert(is_power_of_2(align));
    assert((offset & (align - 1)) == 0);
    assert((bytes & (align - 1)) == 0);
    assert(!qiov || bytes == qiov->size);
    assert((bs->open_flags & BDRV_O_NO_IO) == 0);
    max_transfer = QEMU_ALIGN_DOWN(MIN_NON_ZERO(bs->bl.max_transfer, INT_MAX),
                                   align);

    /* TODO: We would need a per-BDS .supported_read_flags and
     * potential fallback support, if we ever implement any read flags
     * to pass through to drivers.  For now, there aren't any
     * passthrough flags.  */
    assert(!(flags & ~(BDRV_REQ_NO_SERIALISING | BDRV_REQ_COPY_ON_READ)));

    /* Handle Copy on Read and associated serialisation */
    if (flags & BDRV_REQ_COPY_ON_READ) {
        /* If we touch the same cluster it counts as an overlap.  This
         * guarantees that allocating writes will be serialized and not race
         * with each other for the same cluster.  For example, in copy-on-read
         * it ensures that the CoR read and write operations are atomic and
         * guest writes cannot interleave between them. */
        mark_request_serialising(req, bdrv_get_cluster_size(bs));
    }

    if (!(flags & BDRV_REQ_NO_SERIALISING)) {
        wait_serialising_requests(req);
    }

    if (flags & BDRV_REQ_COPY_ON_READ) {
        int64_t pnum;

        ret = bdrv_is_allocated(bs, offset, bytes, &pnum);
        if (ret < 0) {
            goto out;
        }

        if (!ret || pnum != bytes) {
            ret = bdrv_co_do_copy_on_readv(child, offset, bytes, qiov);
            goto out;
        }
    }

    /* Forward the request to the BlockDriver, possibly fragmenting it */
    total_bytes = bdrv_getlength(bs);
    if (total_bytes < 0) {
        ret = total_bytes;
        goto out;
    }

    max_bytes = ROUND_UP(MAX(0, total_bytes - offset), align);
    if (bytes <= max_bytes && bytes <= max_transfer) {
        ret = bdrv_driver_preadv(bs, offset, bytes, qiov, 0);
        goto out;
    }

    while (bytes_remaining) {
        int num;

        if (max_bytes) {
            QEMUIOVector local_qiov;

            num = MIN(bytes_remaining, MIN(max_bytes, max_transfer));
            assert(num);
            qemu_iovec_init(&local_qiov, qiov->niov);
            qemu_iovec_concat(&local_qiov, qiov, bytes - bytes_remaining, num);

            ret = bdrv_driver_preadv(bs, offset + bytes - bytes_remaining,
                                     num, &local_qiov, 0);
            max_bytes -= num;
            qemu_iovec_destroy(&local_qiov);
        } else {
            num = bytes_remaining;
            ret = qemu_iovec_memset(qiov, bytes - bytes_remaining, 0,
                                    bytes_remaining);
        }
        if (ret < 0) {
            goto out;
        }
        bytes_remaining -= num;
    }

out:
    return ret < 0 ? ret : 0;
}

/*
 * Handle a read request in coroutine context
 */
int coroutine_fn bdrv_co_preadv(BdrvChild *child,
    int64_t offset, unsigned int bytes, QEMUIOVector *qiov,
    BdrvRequestFlags flags)
{
    BlockDriverState *bs = child->bs;
    BlockDriver *drv = bs->drv;
    BdrvTrackedRequest req;

    uint64_t align = bs->bl.request_alignment;
    uint8_t *head_buf = NULL;
    uint8_t *tail_buf = NULL;
    QEMUIOVector local_qiov;
    bool use_local_qiov = false;
    int ret;

    trace_bdrv_co_preadv(child->bs, offset, bytes, flags);

    if (!drv) {
        return -ENOMEDIUM;
    }

    ret = bdrv_check_byte_request(bs, offset, bytes);
    if (ret < 0) {
        return ret;
    }

    bdrv_inc_in_flight(bs);

    /* Don't do copy-on-read if we read data before write operation */
    if (atomic_read(&bs->copy_on_read) && !(flags & BDRV_REQ_NO_SERIALISING)) {
        flags |= BDRV_REQ_COPY_ON_READ;
    }

    /* Align read if necessary by padding qiov */
    if (offset & (align - 1)) {
        head_buf = qemu_blockalign(bs, align);
        qemu_iovec_init(&local_qiov, qiov->niov + 2);
        qemu_iovec_add(&local_qiov, head_buf, offset & (align - 1));
        qemu_iovec_concat(&local_qiov, qiov, 0, qiov->size);
        use_local_qiov = true;

        bytes += offset & (align - 1);
        offset = offset & ~(align - 1);
    }

    if ((offset + bytes) & (align - 1)) {
        if (!use_local_qiov) {
            qemu_iovec_init(&local_qiov, qiov->niov + 1);
            qemu_iovec_concat(&local_qiov, qiov, 0, qiov->size);
            use_local_qiov = true;
        }
        tail_buf = qemu_blockalign(bs, align);
        qemu_iovec_add(&local_qiov, tail_buf,
                       align - ((offset + bytes) & (align - 1)));

        bytes = ROUND_UP(bytes, align);
    }

    tracked_request_begin(&req, bs, offset, bytes, BDRV_TRACKED_READ);
    ret = bdrv_aligned_preadv(child, &req, offset, bytes, align,
                              use_local_qiov ? &local_qiov : qiov,
                              flags);
    tracked_request_end(&req);
    bdrv_dec_in_flight(bs);

    if (use_local_qiov) {
        qemu_iovec_destroy(&local_qiov);
        qemu_vfree(head_buf);
        qemu_vfree(tail_buf);
    }

    return ret;
}

static int coroutine_fn bdrv_co_do_readv(BdrvChild *child,
    int64_t sector_num, int nb_sectors, QEMUIOVector *qiov,
    BdrvRequestFlags flags)
{
    if (nb_sectors < 0 || nb_sectors > BDRV_REQUEST_MAX_SECTORS) {
        return -EINVAL;
    }

    return bdrv_co_preadv(child, sector_num << BDRV_SECTOR_BITS,
                          nb_sectors << BDRV_SECTOR_BITS, qiov, flags);
}

int coroutine_fn bdrv_co_readv(BdrvChild *child, int64_t sector_num,
                               int nb_sectors, QEMUIOVector *qiov)
{
    return bdrv_co_do_readv(child, sector_num, nb_sectors, qiov, 0);
}

static int coroutine_fn bdrv_co_do_pwrite_zeroes(BlockDriverState *bs,
    int64_t offset, int bytes, BdrvRequestFlags flags)
{
    BlockDriver *drv = bs->drv;
    QEMUIOVector qiov;
    struct iovec iov = {0};
    int ret = 0;
    bool need_flush = false;
    int head = 0;
    int tail = 0;

    int max_write_zeroes = MIN_NON_ZERO(bs->bl.max_pwrite_zeroes, INT_MAX);
    int alignment = MAX(bs->bl.pwrite_zeroes_alignment,
                        bs->bl.request_alignment);
    int max_transfer = MIN_NON_ZERO(bs->bl.max_transfer, MAX_BOUNCE_BUFFER);

    if (!drv) {
        return -ENOMEDIUM;
    }

    assert(alignment % bs->bl.request_alignment == 0);
    head = offset % alignment;
    tail = (offset + bytes) % alignment;
    max_write_zeroes = QEMU_ALIGN_DOWN(max_write_zeroes, alignment);
    assert(max_write_zeroes >= bs->bl.request_alignment);

    while (bytes > 0 && !ret) {
        int num = bytes;

        /* Align request.  Block drivers can expect the "bulk" of the request
         * to be aligned, and that unaligned requests do not cross cluster
         * boundaries.
         */
        if (head) {
            /* Make a small request up to the first aligned sector. For
             * convenience, limit this request to max_transfer even if
             * we don't need to fall back to writes.  */
            num = MIN(MIN(bytes, max_transfer), alignment - head);
            head = (head + num) % alignment;
            assert(num < max_write_zeroes);
        } else if (tail && num > alignment) {
            /* Shorten the request to the last aligned sector.  */
            num -= tail;
        }

        /* limit request size */
        if (num > max_write_zeroes) {
            num = max_write_zeroes;
        }

        ret = -ENOTSUP;
        /* First try the efficient write zeroes operation */
        if (drv->bdrv_co_pwrite_zeroes) {
            ret = drv->bdrv_co_pwrite_zeroes(bs, offset, num,
                                             flags & bs->supported_zero_flags);
            if (ret != -ENOTSUP && (flags & BDRV_REQ_FUA) &&
                !(bs->supported_zero_flags & BDRV_REQ_FUA)) {
                need_flush = true;
            }
        } else {
            assert(!bs->supported_zero_flags);
        }

        if (ret == -ENOTSUP) {
            /* Fall back to bounce buffer if write zeroes is unsupported */
            BdrvRequestFlags write_flags = flags & ~BDRV_REQ_ZERO_WRITE;

            if ((flags & BDRV_REQ_FUA) &&
                !(bs->supported_write_flags & BDRV_REQ_FUA)) {
                /* No need for bdrv_driver_pwrite() to do a fallback
                 * flush on each chunk; use just one at the end */
                write_flags &= ~BDRV_REQ_FUA;
                need_flush = true;
            }
            num = MIN(num, max_transfer);
            iov.iov_len = num;
            if (iov.iov_base == NULL) {
                iov.iov_base = qemu_try_blockalign(bs, num);
                if (iov.iov_base == NULL) {
                    ret = -ENOMEM;
                    goto fail;
                }
                memset(iov.iov_base, 0, num);
            }
            qemu_iovec_init_external(&qiov, &iov, 1);

            ret = bdrv_driver_pwritev(bs, offset, num, &qiov, write_flags);

            /* Keep bounce buffer around if it is big enough for all
             * all future requests.
             */
            if (num < max_transfer) {
                qemu_vfree(iov.iov_base);
                iov.iov_base = NULL;
            }
        }

        offset += num;
        bytes -= num;
    }

fail:
    if (ret == 0 && need_flush) {
        ret = bdrv_co_flush(bs);
    }
    qemu_vfree(iov.iov_base);
    return ret;
}

/*
 * Forwards an already correctly aligned write request to the BlockDriver,
 * after possibly fragmenting it.
 */
static int coroutine_fn bdrv_aligned_pwritev(BdrvChild *child,
    BdrvTrackedRequest *req, int64_t offset, unsigned int bytes,
    int64_t align, QEMUIOVector *qiov, int flags)
{
    BlockDriverState *bs = child->bs;
    BlockDriver *drv = bs->drv;
    bool waited;
    int ret;

    int64_t end_sector = DIV_ROUND_UP(offset + bytes, BDRV_SECTOR_SIZE);
    uint64_t bytes_remaining = bytes;
    int max_transfer;

    if (!drv) {
        return -ENOMEDIUM;
    }

    if (bdrv_has_readonly_bitmaps(bs)) {
        return -EPERM;
    }

    assert(is_power_of_2(align));
    assert((offset & (align - 1)) == 0);
    assert((bytes & (align - 1)) == 0);
    assert(!qiov || bytes == qiov->size);
    assert((bs->open_flags & BDRV_O_NO_IO) == 0);
    assert(!(flags & ~BDRV_REQ_MASK));
    max_transfer = QEMU_ALIGN_DOWN(MIN_NON_ZERO(bs->bl.max_transfer, INT_MAX),
                                   align);

    waited = wait_serialising_requests(req);
    assert(!waited || !req->serialising);
    assert(req->overlap_offset <= offset);
    assert(offset + bytes <= req->overlap_offset + req->overlap_bytes);
    assert(child->perm & BLK_PERM_WRITE);
    assert(end_sector <= bs->total_sectors || child->perm & BLK_PERM_RESIZE);

    ret = notifier_with_return_list_notify(&bs->before_write_notifiers, req);

    if (!ret && bs->detect_zeroes != BLOCKDEV_DETECT_ZEROES_OPTIONS_OFF &&
        !(flags & BDRV_REQ_ZERO_WRITE) && drv->bdrv_co_pwrite_zeroes &&
        qemu_iovec_is_zero(qiov)) {
        flags |= BDRV_REQ_ZERO_WRITE;
        if (bs->detect_zeroes == BLOCKDEV_DETECT_ZEROES_OPTIONS_UNMAP) {
            flags |= BDRV_REQ_MAY_UNMAP;
        }
    }

    if (ret < 0) {
        /* Do nothing, write notifier decided to fail this request */
    } else if (flags & BDRV_REQ_ZERO_WRITE) {
        bdrv_debug_event(bs, BLKDBG_PWRITEV_ZERO);
        ret = bdrv_co_do_pwrite_zeroes(bs, offset, bytes, flags);
    } else if (flags & BDRV_REQ_WRITE_COMPRESSED) {
        ret = bdrv_driver_pwritev_compressed(bs, offset, bytes, qiov);
    } else if (bytes <= max_transfer) {
        bdrv_debug_event(bs, BLKDBG_PWRITEV);
        ret = bdrv_driver_pwritev(bs, offset, bytes, qiov, flags);
    } else {
        bdrv_debug_event(bs, BLKDBG_PWRITEV);
        while (bytes_remaining) {
            int num = MIN(bytes_remaining, max_transfer);
            QEMUIOVector local_qiov;
            int local_flags = flags;

            assert(num);
            if (num < bytes_remaining && (flags & BDRV_REQ_FUA) &&
                !(bs->supported_write_flags & BDRV_REQ_FUA)) {
                /* If FUA is going to be emulated by flush, we only
                 * need to flush on the last iteration */
                local_flags &= ~BDRV_REQ_FUA;
            }
            qemu_iovec_init(&local_qiov, qiov->niov);
            qemu_iovec_concat(&local_qiov, qiov, bytes - bytes_remaining, num);

            ret = bdrv_driver_pwritev(bs, offset + bytes - bytes_remaining,
                                      num, &local_qiov, local_flags);
            qemu_iovec_destroy(&local_qiov);
            if (ret < 0) {
                break;
            }
            bytes_remaining -= num;
        }
    }
    bdrv_debug_event(bs, BLKDBG_PWRITEV_DONE);

    atomic_inc(&bs->write_gen);
    bdrv_set_dirty(bs, offset, bytes);

    stat64_max(&bs->wr_highest_offset, offset + bytes);

    if (ret >= 0) {
        bs->total_sectors = MAX(bs->total_sectors, end_sector);
        ret = 0;
    }

    return ret;
}

static int coroutine_fn bdrv_co_do_zero_pwritev(BdrvChild *child,
                                                int64_t offset,
                                                unsigned int bytes,
                                                BdrvRequestFlags flags,
                                                BdrvTrackedRequest *req)
{
    BlockDriverState *bs = child->bs;
    uint8_t *buf = NULL;
    QEMUIOVector local_qiov;
    struct iovec iov;
    uint64_t align = bs->bl.request_alignment;
    unsigned int head_padding_bytes, tail_padding_bytes;
    int ret = 0;

    head_padding_bytes = offset & (align - 1);
    tail_padding_bytes = (align - (offset + bytes)) & (align - 1);


    assert(flags & BDRV_REQ_ZERO_WRITE);
    if (head_padding_bytes || tail_padding_bytes) {
        buf = qemu_blockalign(bs, align);
        iov = (struct iovec) {
            .iov_base   = buf,
            .iov_len    = align,
        };
        qemu_iovec_init_external(&local_qiov, &iov, 1);
    }
    if (head_padding_bytes) {
        uint64_t zero_bytes = MIN(bytes, align - head_padding_bytes);

        /* RMW the unaligned part before head. */
        mark_request_serialising(req, align);
        wait_serialising_requests(req);
        bdrv_debug_event(bs, BLKDBG_PWRITEV_RMW_HEAD);
        ret = bdrv_aligned_preadv(child, req, offset & ~(align - 1), align,
                                  align, &local_qiov, 0);
        if (ret < 0) {
            goto fail;
        }
        bdrv_debug_event(bs, BLKDBG_PWRITEV_RMW_AFTER_HEAD);

        memset(buf + head_padding_bytes, 0, zero_bytes);
        ret = bdrv_aligned_pwritev(child, req, offset & ~(align - 1), align,
                                   align, &local_qiov,
                                   flags & ~BDRV_REQ_ZERO_WRITE);
        if (ret < 0) {
            goto fail;
        }
        offset += zero_bytes;
        bytes -= zero_bytes;
    }

    assert(!bytes || (offset & (align - 1)) == 0);
    if (bytes >= align) {
        /* Write the aligned part in the middle. */
        uint64_t aligned_bytes = bytes & ~(align - 1);
        ret = bdrv_aligned_pwritev(child, req, offset, aligned_bytes, align,
                                   NULL, flags);
        if (ret < 0) {
            goto fail;
        }
        bytes -= aligned_bytes;
        offset += aligned_bytes;
    }

    assert(!bytes || (offset & (align - 1)) == 0);
    if (bytes) {
        assert(align == tail_padding_bytes + bytes);
        /* RMW the unaligned part after tail. */
        mark_request_serialising(req, align);
        wait_serialising_requests(req);
        bdrv_debug_event(bs, BLKDBG_PWRITEV_RMW_TAIL);
        ret = bdrv_aligned_preadv(child, req, offset, align,
                                  align, &local_qiov, 0);
        if (ret < 0) {
            goto fail;
        }
        bdrv_debug_event(bs, BLKDBG_PWRITEV_RMW_AFTER_TAIL);

        memset(buf, 0, bytes);
        ret = bdrv_aligned_pwritev(child, req, offset, align, align,
                                   &local_qiov, flags & ~BDRV_REQ_ZERO_WRITE);
    }
fail:
    qemu_vfree(buf);
    return ret;

}

/*
 * Handle a write request in coroutine context
 */
int coroutine_fn bdrv_co_pwritev(BdrvChild *child,
    int64_t offset, unsigned int bytes, QEMUIOVector *qiov,
    BdrvRequestFlags flags)
{
    BlockDriverState *bs = child->bs;
    BdrvTrackedRequest req;
    uint64_t align = bs->bl.request_alignment;
    uint8_t *head_buf = NULL;
    uint8_t *tail_buf = NULL;
    QEMUIOVector local_qiov;
    bool use_local_qiov = false;
    int ret;

    trace_bdrv_co_pwritev(child->bs, offset, bytes, flags);

    if (!bs->drv) {
        return -ENOMEDIUM;
    }
    if (bs->read_only) {
        return -EPERM;
    }
    assert(!(bs->open_flags & BDRV_O_INACTIVE));

    ret = bdrv_check_byte_request(bs, offset, bytes);
    if (ret < 0) {
        return ret;
    }

    bdrv_inc_in_flight(bs);
    /*
     * Align write if necessary by performing a read-modify-write cycle.
     * Pad qiov with the read parts and be sure to have a tracked request not
     * only for bdrv_aligned_pwritev, but also for the reads of the RMW cycle.
     */
    tracked_request_begin(&req, bs, offset, bytes, BDRV_TRACKED_WRITE);

    if (flags & BDRV_REQ_ZERO_WRITE) {
        ret = bdrv_co_do_zero_pwritev(child, offset, bytes, flags, &req);
        goto out;
    }

    if (offset & (align - 1)) {
        QEMUIOVector head_qiov;
        struct iovec head_iov;

        mark_request_serialising(&req, align);
        wait_serialising_requests(&req);

        head_buf = qemu_blockalign(bs, align);
        head_iov = (struct iovec) {
            .iov_base   = head_buf,
            .iov_len    = align,
        };
        qemu_iovec_init_external(&head_qiov, &head_iov, 1);

        bdrv_debug_event(bs, BLKDBG_PWRITEV_RMW_HEAD);
        ret = bdrv_aligned_preadv(child, &req, offset & ~(align - 1), align,
                                  align, &head_qiov, 0);
        if (ret < 0) {
            goto fail;
        }
        bdrv_debug_event(bs, BLKDBG_PWRITEV_RMW_AFTER_HEAD);

        qemu_iovec_init(&local_qiov, qiov->niov + 2);
        qemu_iovec_add(&local_qiov, head_buf, offset & (align - 1));
        qemu_iovec_concat(&local_qiov, qiov, 0, qiov->size);
        use_local_qiov = true;

        bytes += offset & (align - 1);
        offset = offset & ~(align - 1);

        /* We have read the tail already if the request is smaller
         * than one aligned block.
         */
        if (bytes < align) {
            qemu_iovec_add(&local_qiov, head_buf + bytes, align - bytes);
            bytes = align;
        }
    }

    if ((offset + bytes) & (align - 1)) {
        QEMUIOVector tail_qiov;
        struct iovec tail_iov;
        size_t tail_bytes;
        bool waited;

        mark_request_serialising(&req, align);
        waited = wait_serialising_requests(&req);
        assert(!waited || !use_local_qiov);

        tail_buf = qemu_blockalign(bs, align);
        tail_iov = (struct iovec) {
            .iov_base   = tail_buf,
            .iov_len    = align,
        };
        qemu_iovec_init_external(&tail_qiov, &tail_iov, 1);

        bdrv_debug_event(bs, BLKDBG_PWRITEV_RMW_TAIL);
        ret = bdrv_aligned_preadv(child, &req, (offset + bytes) & ~(align - 1),
                                  align, align, &tail_qiov, 0);
        if (ret < 0) {
            goto fail;
        }
        bdrv_debug_event(bs, BLKDBG_PWRITEV_RMW_AFTER_TAIL);

        if (!use_local_qiov) {
            qemu_iovec_init(&local_qiov, qiov->niov + 1);
            qemu_iovec_concat(&local_qiov, qiov, 0, qiov->size);
            use_local_qiov = true;
        }

        tail_bytes = (offset + bytes) & (align - 1);
        qemu_iovec_add(&local_qiov, tail_buf + tail_bytes, align - tail_bytes);

        bytes = ROUND_UP(bytes, align);
    }

    ret = bdrv_aligned_pwritev(child, &req, offset, bytes, align,
                               use_local_qiov ? &local_qiov : qiov,
                               flags);

fail:

    if (use_local_qiov) {
        qemu_iovec_destroy(&local_qiov);
    }
    qemu_vfree(head_buf);
    qemu_vfree(tail_buf);
out:
    tracked_request_end(&req);
    bdrv_dec_in_flight(bs);
    return ret;
}

static int coroutine_fn bdrv_co_do_writev(BdrvChild *child,
    int64_t sector_num, int nb_sectors, QEMUIOVector *qiov,
    BdrvRequestFlags flags)
{
    if (nb_sectors < 0 || nb_sectors > BDRV_REQUEST_MAX_SECTORS) {
        return -EINVAL;
    }

    return bdrv_co_pwritev(child, sector_num << BDRV_SECTOR_BITS,
                           nb_sectors << BDRV_SECTOR_BITS, qiov, flags);
}

int coroutine_fn bdrv_co_writev(BdrvChild *child, int64_t sector_num,
    int nb_sectors, QEMUIOVector *qiov)
{
    return bdrv_co_do_writev(child, sector_num, nb_sectors, qiov, 0);
}

int coroutine_fn bdrv_co_pwrite_zeroes(BdrvChild *child, int64_t offset,
                                       int bytes, BdrvRequestFlags flags)
{
    trace_bdrv_co_pwrite_zeroes(child->bs, offset, bytes, flags);

    if (!(child->bs->open_flags & BDRV_O_UNMAP)) {
        flags &= ~BDRV_REQ_MAY_UNMAP;
    }

    return bdrv_co_pwritev(child, offset, bytes, NULL,
                           BDRV_REQ_ZERO_WRITE | flags);
}

/*
 * Flush ALL BDSes regardless of if they are reachable via a BlkBackend or not.
 */
int bdrv_flush_all(void)
{
    BdrvNextIterator it;
    BlockDriverState *bs = NULL;
    int result = 0;

    for (bs = bdrv_first(&it); bs; bs = bdrv_next(&it)) {
        AioContext *aio_context = bdrv_get_aio_context(bs);
        int ret;

        aio_context_acquire(aio_context);
        ret = bdrv_flush(bs);
        if (ret < 0 && !result) {
            result = ret;
        }
        aio_context_release(aio_context);
    }

    return result;
}


typedef struct BdrvCoBlockStatusData {
    BlockDriverState *bs;
    BlockDriverState *base;
    bool want_zero;
    int64_t offset;
    int64_t bytes;
    int64_t *pnum;
    int64_t *map;
    BlockDriverState **file;
    int ret;
    bool done;
} BdrvCoBlockStatusData;

int coroutine_fn bdrv_co_block_status_from_file(BlockDriverState *bs,
                                                bool want_zero,
                                                int64_t offset,
                                                int64_t bytes,
                                                int64_t *pnum,
                                                int64_t *map,
                                                BlockDriverState **file)
{
    assert(bs->file && bs->file->bs);
    *pnum = bytes;
    *map = offset;
    *file = bs->file->bs;
    return BDRV_BLOCK_RAW | BDRV_BLOCK_OFFSET_VALID;
}

int coroutine_fn bdrv_co_block_status_from_backing(BlockDriverState *bs,
                                                   bool want_zero,
                                                   int64_t offset,
                                                   int64_t bytes,
                                                   int64_t *pnum,
                                                   int64_t *map,
                                                   BlockDriverState **file)
{
    assert(bs->backing && bs->backing->bs);
    *pnum = bytes;
    *map = offset;
    *file = bs->backing->bs;
    return BDRV_BLOCK_RAW | BDRV_BLOCK_OFFSET_VALID;
}

/*
 * Returns the allocation status of the specified sectors.
 * Drivers not implementing the functionality are assumed to not support
 * backing files, hence all their sectors are reported as allocated.
 *
 * If 'want_zero' is true, the caller is querying for mapping
 * purposes, with a focus on valid BDRV_BLOCK_OFFSET_VALID, _DATA, and
 * _ZERO where possible; otherwise, the result favors larger 'pnum',
 * with a focus on accurate BDRV_BLOCK_ALLOCATED.
 *
 * If 'offset' is beyond the end of the disk image the return value is
 * BDRV_BLOCK_EOF and 'pnum' is set to 0.
 *
 * 'bytes' is the max value 'pnum' should be set to.  If bytes goes
 * beyond the end of the disk image it will be clamped; if 'pnum' is set to
 * the end of the image, then the returned value will include BDRV_BLOCK_EOF.
 *
 * 'pnum' is set to the number of bytes (including and immediately
 * following the specified offset) that are easily known to be in the
 * same allocated/unallocated state.  Note that a second call starting
 * at the original offset plus returned pnum may have the same status.
 * The returned value is non-zero on success except at end-of-file.
 *
 * Returns negative errno on failure.  Otherwise, if the
 * BDRV_BLOCK_OFFSET_VALID bit is set, 'map' and 'file' (if non-NULL) are
 * set to the host mapping and BDS corresponding to the guest offset.
 */
static int coroutine_fn bdrv_co_block_status(BlockDriverState *bs,
                                             bool want_zero,
                                             int64_t offset, int64_t bytes,
                                             int64_t *pnum, int64_t *map,
                                             BlockDriverState **file)
{
    int64_t total_size;
    int64_t n; /* bytes */
    int ret;
    int64_t local_map = 0;
    BlockDriverState *local_file = NULL;
    int64_t aligned_offset, aligned_bytes;
    uint32_t align;

    assert(pnum);
    *pnum = 0;
    total_size = bdrv_getlength(bs);
    if (total_size < 0) {
        ret = total_size;
        goto early_out;
    }

    if (offset >= total_size) {
        ret = BDRV_BLOCK_EOF;
        goto early_out;
    }
    if (!bytes) {
        ret = 0;
        goto early_out;
    }

    n = total_size - offset;
    if (n < bytes) {
        bytes = n;
    }

    /* Must be non-NULL or bdrv_getlength() would have failed */
    assert(bs->drv);
    if (!bs->drv->bdrv_co_block_status) {
        *pnum = bytes;
        ret = BDRV_BLOCK_DATA | BDRV_BLOCK_ALLOCATED;
        if (offset + bytes == total_size) {
            ret |= BDRV_BLOCK_EOF;
        }
        if (bs->drv->protocol_name) {
            ret |= BDRV_BLOCK_OFFSET_VALID;
            local_map = offset;
            local_file = bs;
        }
        goto early_out;
    }

    bdrv_inc_in_flight(bs);

    /* Round out to request_alignment boundaries */
    align = bs->bl.request_alignment;
    aligned_offset = QEMU_ALIGN_DOWN(offset, align);
    aligned_bytes = ROUND_UP(offset + bytes, align) - aligned_offset;

    ret = bs->drv->bdrv_co_block_status(bs, want_zero, aligned_offset,
                                        aligned_bytes, pnum, &local_map,
                                        &local_file);
    if (ret < 0) {
        *pnum = 0;
        goto out;
    }

    /*
     * The driver's result must be a non-zero multiple of request_alignment.
     * Clamp pnum and adjust map to original request.
     */
    assert(*pnum && QEMU_IS_ALIGNED(*pnum, align) &&
           align > offset - aligned_offset);
    *pnum -= offset - aligned_offset;
    if (*pnum > bytes) {
        *pnum = bytes;
    }
    if (ret & BDRV_BLOCK_OFFSET_VALID) {
        local_map += offset - aligned_offset;
    }

    if (ret & BDRV_BLOCK_RAW) {
        assert(ret & BDRV_BLOCK_OFFSET_VALID && local_file);
        ret = bdrv_co_block_status(local_file, want_zero, local_map,
                                   *pnum, pnum, &local_map, &local_file);
        goto out;
    }

    if (ret & (BDRV_BLOCK_DATA | BDRV_BLOCK_ZERO)) {
        ret |= BDRV_BLOCK_ALLOCATED;
    } else if (want_zero) {
        if (bdrv_unallocated_blocks_are_zero(bs)) {
            ret |= BDRV_BLOCK_ZERO;
        } else if (bs->backing) {
            BlockDriverState *bs2 = bs->backing->bs;
            int64_t size2 = bdrv_getlength(bs2);

            if (size2 >= 0 && offset >= size2) {
                ret |= BDRV_BLOCK_ZERO;
            }
        }
    }

    if (want_zero && local_file && local_file != bs &&
        (ret & BDRV_BLOCK_DATA) && !(ret & BDRV_BLOCK_ZERO) &&
        (ret & BDRV_BLOCK_OFFSET_VALID)) {
        int64_t file_pnum;
        int ret2;

        ret2 = bdrv_co_block_status(local_file, want_zero, local_map,
                                    *pnum, &file_pnum, NULL, NULL);
        if (ret2 >= 0) {
            /* Ignore errors.  This is just providing extra information, it
             * is useful but not necessary.
             */
            if (ret2 & BDRV_BLOCK_EOF &&
                (!file_pnum || ret2 & BDRV_BLOCK_ZERO)) {
                /*
                 * It is valid for the format block driver to read
                 * beyond the end of the underlying file's current
                 * size; such areas read as zero.
                 */
                ret |= BDRV_BLOCK_ZERO;
            } else {
                /* Limit request to the range reported by the protocol driver */
                *pnum = file_pnum;
                ret |= (ret2 & BDRV_BLOCK_ZERO);
            }
        }
    }

out:
    bdrv_dec_in_flight(bs);
    if (ret >= 0 && offset + *pnum == total_size) {
        ret |= BDRV_BLOCK_EOF;
    }
early_out:
    if (file) {
        *file = local_file;
    }
    if (map) {
        *map = local_map;
    }
    return ret;
}

static int coroutine_fn bdrv_co_block_status_above(BlockDriverState *bs,
                                                   BlockDriverState *base,
                                                   bool want_zero,
                                                   int64_t offset,
                                                   int64_t bytes,
                                                   int64_t *pnum,
                                                   int64_t *map,
                                                   BlockDriverState **file)
{
    BlockDriverState *p;
    int ret = 0;
    bool first = true;

    assert(bs != base);
    for (p = bs; p != base; p = backing_bs(p)) {
        ret = bdrv_co_block_status(p, want_zero, offset, bytes, pnum, map,
                                   file);
        if (ret < 0) {
            break;
        }
        if (ret & BDRV_BLOCK_ZERO && ret & BDRV_BLOCK_EOF && !first) {
            /*
             * Reading beyond the end of the file continues to read
             * zeroes, but we can only widen the result to the
             * unallocated length we learned from an earlier
             * iteration.
             */
            *pnum = bytes;
        }
        if (ret & (BDRV_BLOCK_ZERO | BDRV_BLOCK_DATA)) {
            break;
        }
        /* [offset, pnum] unallocated on this layer, which could be only
         * the first part of [offset, bytes].  */
        bytes = MIN(bytes, *pnum);
        first = false;
    }
    return ret;
}

/* Coroutine wrapper for bdrv_block_status_above() */
static void coroutine_fn bdrv_block_status_above_co_entry(void *opaque)
{
    BdrvCoBlockStatusData *data = opaque;

    data->ret = bdrv_co_block_status_above(data->bs, data->base,
                                           data->want_zero,
                                           data->offset, data->bytes,
                                           data->pnum, data->map, data->file);
    data->done = true;
}

/*
 * Synchronous wrapper around bdrv_co_block_status_above().
 *
 * See bdrv_co_block_status_above() for details.
 */
static int bdrv_common_block_status_above(BlockDriverState *bs,
                                          BlockDriverState *base,
                                          bool want_zero, int64_t offset,
                                          int64_t bytes, int64_t *pnum,
                                          int64_t *map,
                                          BlockDriverState **file)
{
    Coroutine *co;
    BdrvCoBlockStatusData data = {
        .bs = bs,
        .base = base,
        .want_zero = want_zero,
        .offset = offset,
        .bytes = bytes,
        .pnum = pnum,
        .map = map,
        .file = file,
        .done = false,
    };

    if (qemu_in_coroutine()) {
        /* Fast-path if already in coroutine context */
        bdrv_block_status_above_co_entry(&data);
    } else {
        co = qemu_coroutine_create(bdrv_block_status_above_co_entry, &data);
        bdrv_coroutine_enter(bs, co);
        BDRV_POLL_WHILE(bs, !data.done);
    }
    return data.ret;
}

int bdrv_block_status_above(BlockDriverState *bs, BlockDriverState *base,
                            int64_t offset, int64_t bytes, int64_t *pnum,
                            int64_t *map, BlockDriverState **file)
{
    return bdrv_common_block_status_above(bs, base, true, offset, bytes,
                                          pnum, map, file);
}

int bdrv_block_status(BlockDriverState *bs, int64_t offset, int64_t bytes,
                      int64_t *pnum, int64_t *map, BlockDriverState **file)
{
    return bdrv_block_status_above(bs, backing_bs(bs),
                                   offset, bytes, pnum, map, file);
}

int coroutine_fn bdrv_is_allocated(BlockDriverState *bs, int64_t offset,
                                   int64_t bytes, int64_t *pnum)
{
    int ret;
    int64_t dummy;

    ret = bdrv_common_block_status_above(bs, backing_bs(bs), false, offset,
                                         bytes, pnum ? pnum : &dummy, NULL,
                                         NULL);
    if (ret < 0) {
        return ret;
    }
    return !!(ret & BDRV_BLOCK_ALLOCATED);
}

/*
 * Given an image chain: ... -> [BASE] -> [INTER1] -> [INTER2] -> [TOP]
 *
 * Return true if (a prefix of) the given range is allocated in any image
 * between BASE and TOP (inclusive).  BASE can be NULL to check if the given
 * offset is allocated in any image of the chain.  Return false otherwise,
 * or negative errno on failure.
 *
 * 'pnum' is set to the number of bytes (including and immediately
 * following the specified offset) that are known to be in the same
 * allocated/unallocated state.  Note that a subsequent call starting
 * at 'offset + *pnum' may return the same allocation status (in other
 * words, the result is not necessarily the maximum possible range);
 * but 'pnum' will only be 0 when end of file is reached.
 *
 */
int bdrv_is_allocated_above(BlockDriverState *top,
                            BlockDriverState *base,
                            int64_t offset, int64_t bytes, int64_t *pnum)
{
    BlockDriverState *intermediate;
    int ret;
    int64_t n = bytes;

    intermediate = top;
    while (intermediate && intermediate != base) {
        int64_t pnum_inter;
        int64_t size_inter;

        ret = bdrv_is_allocated(intermediate, offset, bytes, &pnum_inter);
        if (ret < 0) {
            return ret;
        }
        if (ret) {
            *pnum = pnum_inter;
            return 1;
        }

        size_inter = bdrv_getlength(intermediate);
        if (size_inter < 0) {
            return size_inter;
        }
        if (n > pnum_inter &&
            (intermediate == top || offset + pnum_inter < size_inter)) {
            n = pnum_inter;
        }

        intermediate = backing_bs(intermediate);
    }

    *pnum = n;
    return 0;
}

typedef struct BdrvVmstateCo {
    BlockDriverState    *bs;
    QEMUIOVector        *qiov;
    int64_t             pos;
    bool                is_read;
    int                 ret;
} BdrvVmstateCo;

static int coroutine_fn
bdrv_co_rw_vmstate(BlockDriverState *bs, QEMUIOVector *qiov, int64_t pos,
                   bool is_read)
{
    BlockDriver *drv = bs->drv;
    int ret = -ENOTSUP;

    bdrv_inc_in_flight(bs);

    if (!drv) {
        ret = -ENOMEDIUM;
    } else if (drv->bdrv_load_vmstate) {
        if (is_read) {
            ret = drv->bdrv_load_vmstate(bs, qiov, pos);
        } else {
            ret = drv->bdrv_save_vmstate(bs, qiov, pos);
        }
    } else if (bs->file) {
        ret = bdrv_co_rw_vmstate(bs->file->bs, qiov, pos, is_read);
    }

    bdrv_dec_in_flight(bs);
    return ret;
}

static void coroutine_fn bdrv_co_rw_vmstate_entry(void *opaque)
{
    BdrvVmstateCo *co = opaque;
    co->ret = bdrv_co_rw_vmstate(co->bs, co->qiov, co->pos, co->is_read);
}

static inline int
bdrv_rw_vmstate(BlockDriverState *bs, QEMUIOVector *qiov, int64_t pos,
                bool is_read)
{
    if (qemu_in_coroutine()) {
        return bdrv_co_rw_vmstate(bs, qiov, pos, is_read);
    } else {
        BdrvVmstateCo data = {
            .bs         = bs,
            .qiov       = qiov,
            .pos        = pos,
            .is_read    = is_read,
            .ret        = -EINPROGRESS,
        };
        Coroutine *co = qemu_coroutine_create(bdrv_co_rw_vmstate_entry, &data);

        bdrv_coroutine_enter(bs, co);
        BDRV_POLL_WHILE(bs, data.ret == -EINPROGRESS);
        return data.ret;
    }
}

int bdrv_save_vmstate(BlockDriverState *bs, const uint8_t *buf,
                      int64_t pos, int size)
{
    QEMUIOVector qiov;
    struct iovec iov = {
        .iov_base   = (void *) buf,
        .iov_len    = size,
    };
    int ret;

    qemu_iovec_init_external(&qiov, &iov, 1);

    ret = bdrv_writev_vmstate(bs, &qiov, pos);
    if (ret < 0) {
        return ret;
    }

    return size;
}

int bdrv_writev_vmstate(BlockDriverState *bs, QEMUIOVector *qiov, int64_t pos)
{
    return bdrv_rw_vmstate(bs, qiov, pos, false);
}

int bdrv_load_vmstate(BlockDriverState *bs, uint8_t *buf,
                      int64_t pos, int size)
{
    QEMUIOVector qiov;
    struct iovec iov = {
        .iov_base   = buf,
        .iov_len    = size,
    };
    int ret;

    qemu_iovec_init_external(&qiov, &iov, 1);
    ret = bdrv_readv_vmstate(bs, &qiov, pos);
    if (ret < 0) {
        return ret;
    }

    return size;
}

int bdrv_readv_vmstate(BlockDriverState *bs, QEMUIOVector *qiov, int64_t pos)
{
    return bdrv_rw_vmstate(bs, qiov, pos, true);
}

/**************************************************************/
/* async I/Os */

void bdrv_aio_cancel(BlockAIOCB *acb)
{
    qemu_aio_ref(acb);
    bdrv_aio_cancel_async(acb);
    while (acb->refcnt > 1) {
        if (acb->aiocb_info->get_aio_context) {
            aio_poll(acb->aiocb_info->get_aio_context(acb), true);
        } else if (acb->bs) {
            /* qemu_aio_ref and qemu_aio_unref are not thread-safe, so
             * assert that we're not using an I/O thread.  Thread-safe
             * code should use bdrv_aio_cancel_async exclusively.
             */
            assert(bdrv_get_aio_context(acb->bs) == qemu_get_aio_context());
            aio_poll(bdrv_get_aio_context(acb->bs), true);
        } else {
            abort();
        }
    }
    qemu_aio_unref(acb);
}

/* Async version of aio cancel. The caller is not blocked if the acb implements
 * cancel_async, otherwise we do nothing and let the request normally complete.
 * In either case the completion callback must be called. */
void bdrv_aio_cancel_async(BlockAIOCB *acb)
{
    if (acb->aiocb_info->cancel_async) {
        acb->aiocb_info->cancel_async(acb);
    }
}

/**************************************************************/
/* Coroutine block device emulation */

typedef struct FlushCo {
    BlockDriverState *bs;
    int ret;
} FlushCo;


static void coroutine_fn bdrv_flush_co_entry(void *opaque)
{
    FlushCo *rwco = opaque;

    rwco->ret = bdrv_co_flush(rwco->bs);
}

int coroutine_fn bdrv_co_flush(BlockDriverState *bs)
{
    int current_gen;
    int ret = 0;

    bdrv_inc_in_flight(bs);

    if (!bdrv_is_inserted(bs) || bdrv_is_read_only(bs) ||
        bdrv_is_sg(bs)) {
        goto early_exit;
    }

    qemu_co_mutex_lock(&bs->reqs_lock);
    current_gen = atomic_read(&bs->write_gen);

    /* Wait until any previous flushes are completed */
    while (bs->active_flush_req) {
        qemu_co_queue_wait(&bs->flush_queue, &bs->reqs_lock);
    }

    /* Flushes reach this point in nondecreasing current_gen order.  */
    bs->active_flush_req = true;
    qemu_co_mutex_unlock(&bs->reqs_lock);

    /* Write back all layers by calling one driver function */
    if (bs->drv->bdrv_co_flush) {
        ret = bs->drv->bdrv_co_flush(bs);
        goto out;
    }

    /* Write back cached data to the OS even with cache=unsafe */
    BLKDBG_EVENT(bs->file, BLKDBG_FLUSH_TO_OS);
    if (bs->drv->bdrv_co_flush_to_os) {
        ret = bs->drv->bdrv_co_flush_to_os(bs);
        if (ret < 0) {
            goto out;
        }
    }

    /* But don't actually force it to the disk with cache=unsafe */
    if (bs->open_flags & BDRV_O_NO_FLUSH) {
        goto flush_parent;
    }

    /* Check if we really need to flush anything */
    if (bs->flushed_gen == current_gen) {
        goto flush_parent;
    }

    BLKDBG_EVENT(bs->file, BLKDBG_FLUSH_TO_DISK);
    if (!bs->drv) {
        /* bs->drv->bdrv_co_flush() might have ejected the BDS
         * (even in case of apparent success) */
        ret = -ENOMEDIUM;
        goto out;
    }
    if (bs->drv->bdrv_co_flush_to_disk) {
        ret = bs->drv->bdrv_co_flush_to_disk(bs);
    } else if (bs->drv->bdrv_aio_flush) {
        BlockAIOCB *acb;
        CoroutineIOCompletion co = {
            .coroutine = qemu_coroutine_self(),
        };

        acb = bs->drv->bdrv_aio_flush(bs, bdrv_co_io_em_complete, &co);
        if (acb == NULL) {
            ret = -EIO;
        } else {
            qemu_coroutine_yield();
            ret = co.ret;
        }
    } else {
        /*
         * Some block drivers always operate in either writethrough or unsafe
         * mode and don't support bdrv_flush therefore. Usually qemu doesn't
         * know how the server works (because the behaviour is hardcoded or
         * depends on server-side configuration), so we can't ensure that
         * everything is safe on disk. Returning an error doesn't work because
         * that would break guests even if the server operates in writethrough
         * mode.
         *
         * Let's hope the user knows what he's doing.
         */
        ret = 0;
    }

    if (ret < 0) {
        goto out;
    }

    /* Now flush the underlying protocol.  It will also have BDRV_O_NO_FLUSH
     * in the case of cache=unsafe, so there are no useless flushes.
     */
flush_parent:
    ret = bs->file ? bdrv_co_flush(bs->file->bs) : 0;
out:
    /* Notify any pending flushes that we have completed */
    if (ret == 0) {
        bs->flushed_gen = current_gen;
    }

    qemu_co_mutex_lock(&bs->reqs_lock);
    bs->active_flush_req = false;
    /* Return value is ignored - it's ok if wait queue is empty */
    qemu_co_queue_next(&bs->flush_queue);
    qemu_co_mutex_unlock(&bs->reqs_lock);

early_exit:
    bdrv_dec_in_flight(bs);
    return ret;
}

int bdrv_flush(BlockDriverState *bs)
{
    Coroutine *co;
    FlushCo flush_co = {
        .bs = bs,
        .ret = NOT_DONE,
    };

    if (qemu_in_coroutine()) {
        /* Fast-path if already in coroutine context */
        bdrv_flush_co_entry(&flush_co);
    } else {
        co = qemu_coroutine_create(bdrv_flush_co_entry, &flush_co);
        bdrv_coroutine_enter(bs, co);
        BDRV_POLL_WHILE(bs, flush_co.ret == NOT_DONE);
    }

    return flush_co.ret;
}

typedef struct DiscardCo {
    BlockDriverState *bs;
    int64_t offset;
    int bytes;
    int ret;
} DiscardCo;
static void coroutine_fn bdrv_pdiscard_co_entry(void *opaque)
{
    DiscardCo *rwco = opaque;

    rwco->ret = bdrv_co_pdiscard(rwco->bs, rwco->offset, rwco->bytes);
}

int coroutine_fn bdrv_co_pdiscard(BlockDriverState *bs, int64_t offset,
                                  int bytes)
{
    BdrvTrackedRequest req;
    int max_pdiscard, ret;
    int head, tail, align;

    if (!bs->drv) {
        return -ENOMEDIUM;
    }

    if (bdrv_has_readonly_bitmaps(bs)) {
        return -EPERM;
    }

    ret = bdrv_check_byte_request(bs, offset, bytes);
    if (ret < 0) {
        return ret;
    } else if (bs->read_only) {
        return -EPERM;
    }
    assert(!(bs->open_flags & BDRV_O_INACTIVE));

    /* Do nothing if disabled.  */
    if (!(bs->open_flags & BDRV_O_UNMAP)) {
        return 0;
    }

    if (!bs->drv->bdrv_co_pdiscard && !bs->drv->bdrv_aio_pdiscard) {
        return 0;
    }

    /* Discard is advisory, but some devices track and coalesce
     * unaligned requests, so we must pass everything down rather than
     * round here.  Still, most devices will just silently ignore
     * unaligned requests (by returning -ENOTSUP), so we must fragment
     * the request accordingly.  */
    align = MAX(bs->bl.pdiscard_alignment, bs->bl.request_alignment);
    assert(align % bs->bl.request_alignment == 0);
    head = offset % align;
    tail = (offset + bytes) % align;

    bdrv_inc_in_flight(bs);
    tracked_request_begin(&req, bs, offset, bytes, BDRV_TRACKED_DISCARD);

    ret = notifier_with_return_list_notify(&bs->before_write_notifiers, &req);
    if (ret < 0) {
        goto out;
    }

    max_pdiscard = QEMU_ALIGN_DOWN(MIN_NON_ZERO(bs->bl.max_pdiscard, INT_MAX),
                                   align);
    assert(max_pdiscard >= bs->bl.request_alignment);

    while (bytes > 0) {
        int num = bytes;

        if (head) {
            /* Make small requests to get to alignment boundaries. */
            num = MIN(bytes, align - head);
            if (!QEMU_IS_ALIGNED(num, bs->bl.request_alignment)) {
                num %= bs->bl.request_alignment;
            }
            head = (head + num) % align;
            assert(num < max_pdiscard);
        } else if (tail) {
            if (num > align) {
                /* Shorten the request to the last aligned cluster.  */
                num -= tail;
            } else if (!QEMU_IS_ALIGNED(tail, bs->bl.request_alignment) &&
                       tail > bs->bl.request_alignment) {
                tail %= bs->bl.request_alignment;
                num -= tail;
            }
        }
        /* limit request size */
        if (num > max_pdiscard) {
            num = max_pdiscard;
        }

        if (!bs->drv) {
            ret = -ENOMEDIUM;
            goto out;
        }
        if (bs->drv->bdrv_co_pdiscard) {
            ret = bs->drv->bdrv_co_pdiscard(bs, offset, num);
        } else {
            BlockAIOCB *acb;
            CoroutineIOCompletion co = {
                .coroutine = qemu_coroutine_self(),
            };

            acb = bs->drv->bdrv_aio_pdiscard(bs, offset, num,
                                             bdrv_co_io_em_complete, &co);
            if (acb == NULL) {
                ret = -EIO;
                goto out;
            } else {
                qemu_coroutine_yield();
                ret = co.ret;
            }
        }
        if (ret && ret != -ENOTSUP) {
            goto out;
        }

        offset += num;
        bytes -= num;
    }
    ret = 0;
out:
    atomic_inc(&bs->write_gen);
    bdrv_set_dirty(bs, req.offset, req.bytes);
    tracked_request_end(&req);
    bdrv_dec_in_flight(bs);
    return ret;
}

int bdrv_pdiscard(BlockDriverState *bs, int64_t offset, int bytes)
{
    Coroutine *co;
    DiscardCo rwco = {
        .bs = bs,
        .offset = offset,
        .bytes = bytes,
        .ret = NOT_DONE,
    };

    if (qemu_in_coroutine()) {
        /* Fast-path if already in coroutine context */
        bdrv_pdiscard_co_entry(&rwco);
    } else {
        co = qemu_coroutine_create(bdrv_pdiscard_co_entry, &rwco);
        bdrv_coroutine_enter(bs, co);
        BDRV_POLL_WHILE(bs, rwco.ret == NOT_DONE);
    }

    return rwco.ret;
}

int bdrv_co_ioctl(BlockDriverState *bs, int req, void *buf)
{
    BlockDriver *drv = bs->drv;
    CoroutineIOCompletion co = {
        .coroutine = qemu_coroutine_self(),
    };
    BlockAIOCB *acb;

    bdrv_inc_in_flight(bs);
    if (!drv || (!drv->bdrv_aio_ioctl && !drv->bdrv_co_ioctl)) {
        co.ret = -ENOTSUP;
        goto out;
    }

    if (drv->bdrv_co_ioctl) {
        co.ret = drv->bdrv_co_ioctl(bs, req, buf);
    } else {
        acb = drv->bdrv_aio_ioctl(bs, req, buf, bdrv_co_io_em_complete, &co);
        if (!acb) {
            co.ret = -ENOTSUP;
            goto out;
        }
        qemu_coroutine_yield();
    }
out:
    bdrv_dec_in_flight(bs);
    return co.ret;
}

void *qemu_blockalign(BlockDriverState *bs, size_t size)
{
    return qemu_memalign(bdrv_opt_mem_align(bs), size);
}

void *qemu_blockalign0(BlockDriverState *bs, size_t size)
{
    return memset(qemu_blockalign(bs, size), 0, size);
}

void *qemu_try_blockalign(BlockDriverState *bs, size_t size)
{
    size_t align = bdrv_opt_mem_align(bs);

    /* Ensure that NULL is never returned on success */
    assert(align > 0);
    if (size == 0) {
        size = align;
    }

    return qemu_try_memalign(align, size);
}

void *qemu_try_blockalign0(BlockDriverState *bs, size_t size)
{
    void *mem = qemu_try_blockalign(bs, size);

    if (mem) {
        memset(mem, 0, size);
    }

    return mem;
}

/*
 * Check if all memory in this vector is sector aligned.
 */
bool bdrv_qiov_is_aligned(BlockDriverState *bs, QEMUIOVector *qiov)
{
    int i;
    size_t alignment = bdrv_min_mem_align(bs);

    for (i = 0; i < qiov->niov; i++) {
        if ((uintptr_t) qiov->iov[i].iov_base % alignment) {
            return false;
        }
        if (qiov->iov[i].iov_len % alignment) {
            return false;
        }
    }

    return true;
}

void bdrv_add_before_write_notifier(BlockDriverState *bs,
                                    NotifierWithReturn *notifier)
{
    notifier_with_return_list_add(&bs->before_write_notifiers, notifier);
}

void bdrv_io_plug(BlockDriverState *bs)
{
    BdrvChild *child;

    QLIST_FOREACH(child, &bs->children, next) {
        bdrv_io_plug(child->bs);
    }

    if (atomic_fetch_inc(&bs->io_plugged) == 0) {
        BlockDriver *drv = bs->drv;
        if (drv && drv->bdrv_io_plug) {
            drv->bdrv_io_plug(bs);
        }
    }
}

void bdrv_io_unplug(BlockDriverState *bs)
{
    BdrvChild *child;

    assert(bs->io_plugged);
    if (atomic_fetch_dec(&bs->io_plugged) == 1) {
        BlockDriver *drv = bs->drv;
        if (drv && drv->bdrv_io_unplug) {
            drv->bdrv_io_unplug(bs);
        }
    }

    QLIST_FOREACH(child, &bs->children, next) {
        bdrv_io_unplug(child->bs);
    }
}

void bdrv_register_buf(BlockDriverState *bs, void *host, size_t size)
{
    BdrvChild *child;

    if (bs->drv && bs->drv->bdrv_register_buf) {
        bs->drv->bdrv_register_buf(bs, host, size);
    }
    QLIST_FOREACH(child, &bs->children, next) {
        bdrv_register_buf(child->bs, host, size);
    }
}

void bdrv_unregister_buf(BlockDriverState *bs, void *host)
{
    BdrvChild *child;

    if (bs->drv && bs->drv->bdrv_unregister_buf) {
        bs->drv->bdrv_unregister_buf(bs, host);
    }
    QLIST_FOREACH(child, &bs->children, next) {
        bdrv_unregister_buf(child->bs, host);
    }
}
