/*
 * Declarations for long-running block device operations
 *
 * Copyright (c) 2011 IBM Corp.
 * Copyright (c) 2012 Red Hat, Inc.
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

#ifndef BLOCKJOB_INT_H
#define BLOCKJOB_INT_H

#include "block/blockjob.h"
#include "block/block.h"

/**
 * BlockJobDriver:
 *
 * A class type for block job driver.
 */
struct BlockJobDriver {
    /** Derived BlockJob struct size */
    size_t instance_size;

    /** String describing the operation, part of query-block-jobs QMP API */
    BlockJobType job_type;

    /** Optional callback for job types that support setting a speed limit */
    void (*set_speed)(BlockJob *job, int64_t speed, Error **errp);

    /** Mandatory: Entrypoint for the Coroutine. */
    CoroutineEntry *start;

    /**
     * Optional callback for job types whose completion must be triggered
     * manually.
     */
    void (*complete)(BlockJob *job, Error **errp);

    /**
     * If the callback is not NULL, prepare will be invoked when all the jobs
     * belonging to the same transaction complete; or upon this job's completion
     * if it is not in a transaction.
     *
     * This callback will not be invoked if the job has already failed.
     * If it fails, abort and then clean will be called.
     */
    int (*prepare)(BlockJob *job);

    /**
     * If the callback is not NULL, it will be invoked when all the jobs
     * belonging to the same transaction complete; or upon this job's
     * completion if it is not in a transaction. Skipped if NULL.
     *
     * All jobs will complete with a call to either .commit() or .abort() but
     * never both.
     */
    void (*commit)(BlockJob *job);

    /**
     * If the callback is not NULL, it will be invoked when any job in the
     * same transaction fails; or upon this job's failure (due to error or
     * cancellation) if it is not in a transaction. Skipped if NULL.
     *
     * All jobs will complete with a call to either .commit() or .abort() but
     * never both.
     */
    void (*abort)(BlockJob *job);

    /**
     * If the callback is not NULL, it will be invoked after a call to either
     * .commit() or .abort(). Regardless of which callback is invoked after
     * completion, .clean() will always be called, even if the job does not
     * belong to a transaction group.
     */
    void (*clean)(BlockJob *job);

    /**
     * If the callback is not NULL, it will be invoked when the job transitions
     * into the paused state.  Paused jobs must not perform any asynchronous
     * I/O or event loop activity.  This callback is used to quiesce jobs.
     */
    void coroutine_fn (*pause)(BlockJob *job);

    /**
     * If the callback is not NULL, it will be invoked when the job transitions
     * out of the paused state.  Any asynchronous I/O or event loop activity
     * should be restarted from this callback.
     */
    void coroutine_fn (*resume)(BlockJob *job);

    /*
     * If the callback is not NULL, it will be invoked before the job is
     * resumed in a new AioContext.  This is the place to move any resources
     * besides job->blk to the new AioContext.
     */
    void (*attached_aio_context)(BlockJob *job, AioContext *new_context);

    /*
     * If the callback is not NULL, it will be invoked when the job has to be
     * synchronously cancelled or completed; it should drain BlockDriverStates
     * as required to ensure progress.
     */
    void (*drain)(BlockJob *job);
};

/**
 * block_job_create:
 * @job_id: The id of the newly-created job, or %NULL to have one
 * generated automatically.
 * @driver: The class object for the newly-created job.
 * @txn: The transaction this job belongs to, if any. %NULL otherwise.
 * @bs: The block
 * @perm, @shared_perm: Permissions to request for @bs
 * @speed: The maximum speed, in bytes per second, or 0 for unlimited.
 * @flags: Creation flags for the Block Job.
 *         See @BlockJobCreateFlags
 * @cb: Completion function for the job.
 * @opaque: Opaque pointer value passed to @cb.
 * @errp: Error object.
 *
 * Create a new long-running block device job and return it.  The job
 * will call @cb asynchronously when the job completes.  Note that
 * @bs may have been closed at the time the @cb it is called.  If
 * this is the case, the job may be reported as either cancelled or
 * completed.
 *
 * This function is not part of the public job interface; it should be
 * called from a wrapper that is specific to the job type.
 */
void *block_job_create(const char *job_id, const BlockJobDriver *driver,
                       BlockJobTxn *txn, BlockDriverState *bs, uint64_t perm,
                       uint64_t shared_perm, int64_t speed, int flags,
                       BlockCompletionFunc *cb, void *opaque, Error **errp);

/**
 * block_job_sleep_ns:
 * @job: The job that calls the function.
 * @ns: How many nanoseconds to stop for.
 *
 * Put the job to sleep (assuming that it wasn't canceled) for @ns
 * %QEMU_CLOCK_REALTIME nanoseconds.  Canceling the job will immediately
 * interrupt the wait.
 */
void block_job_sleep_ns(BlockJob *job, int64_t ns);

/**
 * block_job_yield:
 * @job: The job that calls the function.
 *
 * Yield the block job coroutine.
 */
void block_job_yield(BlockJob *job);


/**
 * block_job_early_fail:
 * @bs: The block device.
 *
 * The block job could not be started, free it.
 */
void block_job_early_fail(BlockJob *job);

/**
 * block_job_completed:
 * @job: The job being completed.
 * @ret: The status code.
 *
 * Call the completion function that was registered at creation time, and
 * free @job.
 */
void block_job_completed(BlockJob *job, int ret);

/**
 * block_job_is_cancelled:
 * @job: The job being queried.
 *
 * Returns whether the job is scheduled for cancellation.
 */
bool block_job_is_cancelled(BlockJob *job);

/**
 * block_job_pause_point:
 * @job: The job that is ready to pause.
 *
 * Pause now if block_job_pause() has been called.  Block jobs that perform
 * lots of I/O must call this between requests so that the job can be paused.
 */
void coroutine_fn block_job_pause_point(BlockJob *job);

/**
 * block_job_enter:
 * @job: The job to enter.
 *
 * Continue the specified job by entering the coroutine.
 */
void block_job_enter(BlockJob *job);

/**
 * block_job_event_ready:
 * @job: The job which is now ready to be completed.
 *
 * Send a BLOCK_JOB_READY event for the specified job.
 */
void block_job_event_ready(BlockJob *job);

/**
 * block_job_error_action:
 * @job: The job to signal an error for.
 * @on_err: The error action setting.
 * @is_read: Whether the operation was a read.
 * @error: The error that was reported.
 *
 * Report an I/O error for a block job and possibly stop the VM.  Return the
 * action that was selected based on @on_err and @error.
 */
BlockErrorAction block_job_error_action(BlockJob *job, BlockdevOnError on_err,
                                        int is_read, int error);

typedef void BlockJobDeferToMainLoopFn(BlockJob *job, void *opaque);

/**
 * block_job_defer_to_main_loop:
 * @job: The job
 * @fn: The function to run in the main loop
 * @opaque: The opaque value that is passed to @fn
 *
 * This function must be called by the main job coroutine just before it
 * returns.  @fn is executed in the main loop with the BlockDriverState
 * AioContext acquired.  Block jobs must call bdrv_unref(), bdrv_close(), and
 * anything that uses bdrv_drain_all() in the main loop.
 *
 * The @job AioContext is held while @fn executes.
 */
void block_job_defer_to_main_loop(BlockJob *job,
                                  BlockJobDeferToMainLoopFn *fn,
                                  void *opaque);

#endif
