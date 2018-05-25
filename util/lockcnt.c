/*
 * QemuLockCnt implementation
 *
 * Copyright Red Hat, Inc. 2017
 *
 * Author:
 *   Paolo Bonzini <pbonzini@redhat.com>
 */
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/atomic.h"
#include "trace.h"

#include "qemu/futex.h"

/* On Linux, bits 0-1 are a futex-based lock, bits 2-31 are the counter.
 * For the mutex algorithm see Ulrich Drepper's "Futexes Are Tricky" (ok,
 * this is not the most relaxing citation I could make...).  It is similar
 * to mutex2 in the paper.
 */

#define QEMU_LOCKCNT_STATE_MASK    3
#define QEMU_LOCKCNT_STATE_FREE    0   /* free, uncontended */
#define QEMU_LOCKCNT_STATE_LOCKED  1   /* locked, uncontended */
#define QEMU_LOCKCNT_STATE_WAITING 2   /* locked, contended */

#define QEMU_LOCKCNT_COUNT_STEP    4
#define QEMU_LOCKCNT_COUNT_SHIFT   2

void qemu_lockcnt_init(QemuLockCnt *lockcnt)
{
    lockcnt->count = 0;
}

void qemu_lockcnt_destroy(QemuLockCnt *lockcnt)
{
}

/* *val is the current value of lockcnt->count.
 *
 * If the lock is free, try a cmpxchg from *val to new_if_free; return
 * true and set *val to the old value found by the cmpxchg in
 * lockcnt->count.
 *
 * If the lock is taken, wait for it to be released and return false
 * *without trying again to take the lock*.  Again, set *val to the
 * new value of lockcnt->count.
 *
 * If *waited is true on return, new_if_free's bottom two bits must not
 * be QEMU_LOCKCNT_STATE_LOCKED on subsequent calls, because the caller
 * does not know if there are other waiters.  Furthermore, after *waited
 * is set the caller has effectively acquired the lock.  If it returns
 * with the lock not taken, it must wake another futex waiter.
 */
static bool qemu_lockcnt_cmpxchg_or_wait(QemuLockCnt *lockcnt, int *val,
                                         int new_if_free, bool *waited)
{
    /* Fast path for when the lock is free.  */
    if ((*val & QEMU_LOCKCNT_STATE_MASK) == QEMU_LOCKCNT_STATE_FREE) {
        int expected = *val;

        trace_lockcnt_fast_path_attempt(lockcnt, expected, new_if_free);
        *val = atomic_cmpxchg(&lockcnt->count, expected, new_if_free);
        if (*val == expected) {
            trace_lockcnt_fast_path_success(lockcnt, expected, new_if_free);
            *val = new_if_free;
            return true;
        }
    }

    /* The slow path moves from locked to waiting if necessary, then
     * does a futex wait.  Both steps can be repeated ad nauseam,
     * only getting out of the loop if we can have another shot at the
     * fast path.  Once we can, get out to compute the new destination
     * value for the fast path.
     */
    while ((*val & QEMU_LOCKCNT_STATE_MASK) != QEMU_LOCKCNT_STATE_FREE) {
        if ((*val & QEMU_LOCKCNT_STATE_MASK) == QEMU_LOCKCNT_STATE_LOCKED) {
            int expected = *val;
            int new = expected - QEMU_LOCKCNT_STATE_LOCKED + QEMU_LOCKCNT_STATE_WAITING;

            trace_lockcnt_futex_wait_prepare(lockcnt, expected, new);
            *val = atomic_cmpxchg(&lockcnt->count, expected, new);
            if (*val == expected) {
                *val = new;
            }
            continue;
        }

        if ((*val & QEMU_LOCKCNT_STATE_MASK) == QEMU_LOCKCNT_STATE_WAITING) {
            *waited = true;
            trace_lockcnt_futex_wait(lockcnt, *val);
            qemu_futex_wait(&lockcnt->count, *val);
            *val = atomic_read(&lockcnt->count);
            trace_lockcnt_futex_wait_resume(lockcnt, *val);
            continue;
        }

        abort();
    }
    return false;
}

static void lockcnt_wake(QemuLockCnt *lockcnt)
{
    trace_lockcnt_futex_wake(lockcnt);
    qemu_futex_wake(&lockcnt->count, 1);
}

void qemu_lockcnt_inc(QemuLockCnt *lockcnt)
{
    int val = atomic_read(&lockcnt->count);
    bool waited = false;

    for (;;) {
        if (val >= QEMU_LOCKCNT_COUNT_STEP) {
            int expected = val;
            val = atomic_cmpxchg(&lockcnt->count, val, val + QEMU_LOCKCNT_COUNT_STEP);
            if (val == expected) {
                break;
            }
        } else {
            /* The fast path is (0, unlocked)->(1, unlocked).  */
            if (qemu_lockcnt_cmpxchg_or_wait(lockcnt, &val, QEMU_LOCKCNT_COUNT_STEP,
                                             &waited)) {
                break;
            }
        }
    }

    /* If we were woken by another thread, we should also wake one because
     * we are effectively releasing the lock that was given to us.  This is
     * the case where qemu_lockcnt_lock would leave QEMU_LOCKCNT_STATE_WAITING
     * in the low bits, and qemu_lockcnt_inc_and_unlock would find it and
     * wake someone.
     */
    if (waited) {
        lockcnt_wake(lockcnt);
    }
}

void qemu_lockcnt_dec(QemuLockCnt *lockcnt)
{
    atomic_sub(&lockcnt->count, QEMU_LOCKCNT_COUNT_STEP);
}

/* If the counter is one, decrement it and return locked.  Otherwise do
 * nothing.
 *
 * If the function returns true, it is impossible for the counter to
 * become nonzero until the next qemu_lockcnt_unlock.
 */
bool qemu_lockcnt_dec_if_lock(QemuLockCnt *lockcnt)
{
    int val = atomic_read(&lockcnt->count);
    int locked_state = QEMU_LOCKCNT_STATE_LOCKED;
    bool waited = false;

    while (val < 2 * QEMU_LOCKCNT_COUNT_STEP) {
        /* If count is going 1->0, take the lock. The fast path is
         * (1, unlocked)->(0, locked) or (1, unlocked)->(0, waiting).
         */
        if (qemu_lockcnt_cmpxchg_or_wait(lockcnt, &val, locked_state, &waited)) {
            return true;
        }

        if (waited) {
            /* At this point we do not know if there are more waiters.  Assume
             * there are.
             */
            locked_state = QEMU_LOCKCNT_STATE_WAITING;
        }
    }

    /* If we were woken by another thread, but we're returning in unlocked
     * state, we should also wake a thread because we are effectively
     * releasing the lock that was given to us.  This is the case where
     * qemu_lockcnt_lock would leave QEMU_LOCKCNT_STATE_WAITING in the low
     * bits, and qemu_lockcnt_inc_and_unlock would find it and wake someone.
     */
    if (waited) {
        lockcnt_wake(lockcnt);
    }
    return false;
}

void qemu_lockcnt_lock(QemuLockCnt *lockcnt)
{
    int val = atomic_read(&lockcnt->count);
    int step = QEMU_LOCKCNT_STATE_LOCKED;
    bool waited = false;

    /* The third argument is only used if the low bits of val are 0
     * (QEMU_LOCKCNT_STATE_FREE), so just blindly mix in the desired
     * state.
     */
    while (!qemu_lockcnt_cmpxchg_or_wait(lockcnt, &val, val + step, &waited)) {
        if (waited) {
            /* At this point we do not know if there are more waiters.  Assume
             * there are.
             */
            step = QEMU_LOCKCNT_STATE_WAITING;
        }
    }
}

void qemu_lockcnt_inc_and_unlock(QemuLockCnt *lockcnt)
{
    int expected, new, val;

    val = atomic_read(&lockcnt->count);
    do {
        expected = val;
        new = (val + QEMU_LOCKCNT_COUNT_STEP) & ~QEMU_LOCKCNT_STATE_MASK;
        trace_lockcnt_unlock_attempt(lockcnt, val, new);
        val = atomic_cmpxchg(&lockcnt->count, val, new);
    } while (val != expected);

    trace_lockcnt_unlock_success(lockcnt, val, new);
    if (val & QEMU_LOCKCNT_STATE_WAITING) {
        lockcnt_wake(lockcnt);
    }
}

void qemu_lockcnt_unlock(QemuLockCnt *lockcnt)
{
    int expected, new, val;

    val = atomic_read(&lockcnt->count);
    do {
        expected = val;
        new = val & ~QEMU_LOCKCNT_STATE_MASK;
        trace_lockcnt_unlock_attempt(lockcnt, val, new);
        val = atomic_cmpxchg(&lockcnt->count, val, new);
    } while (val != expected);

    trace_lockcnt_unlock_success(lockcnt, val, new);
    if (val & QEMU_LOCKCNT_STATE_WAITING) {
        lockcnt_wake(lockcnt);
    }
}

unsigned qemu_lockcnt_count(QemuLockCnt *lockcnt)
{
    return atomic_read(&lockcnt->count) >> QEMU_LOCKCNT_COUNT_SHIFT;
}
