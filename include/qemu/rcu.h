#ifndef QEMU_RCU_H
#define QEMU_RCU_H

/*
 * urcu-mb.h
 *
 * Userspace RCU header with explicit memory barrier.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * IBM's contributions to this file may be relicensed under LGPLv2 or later.
 */


#include "qemu/thread.h"
#include "qemu/queue.h"
#include "qemu/atomic.h"
#include "qemu/sys_membarrier.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Important !
 *
 * Each thread containing read-side critical sections must be registered
 * with rcu_register_thread() before calling rcu_read_lock().
 * rcu_unregister_thread() should be called before the thread exits.
 */

#ifdef DEBUG_RCU
#define rcu_assert(args...)    assert(args)
#else
#define rcu_assert(args...)
#endif

/*
 * Global quiescent period counter with low-order bits unused.
 * Using a int rather than a char to eliminate false register dependencies
 * causing stalls on some architectures.
 */
extern unsigned long rcu_gp_ctr;

extern QemuEvent rcu_gp_event;

struct rcu_reader_data {
    /* Data used by both reader and synchronize_rcu() */
    unsigned long ctr;
    bool waiting;

    /* Data used by reader only */
    unsigned depth;

    /* Data used for registry, protected by rcu_registry_lock */
    QLIST_ENTRY(rcu_reader_data) node;
};

extern __thread struct rcu_reader_data rcu_reader;

static inline void rcu_read_lock(void)
{
    struct rcu_reader_data *p_rcu_reader = &rcu_reader;
    unsigned ctr;

    if (p_rcu_reader->depth++ > 0) {
        return;
    }

    ctr = atomic_read(&rcu_gp_ctr);
    atomic_set(&p_rcu_reader->ctr, ctr);

    /* Write p_rcu_reader->ctr before reading RCU-protected pointers.  */
    smp_mb_placeholder();
}

static inline void rcu_read_unlock(void)
{
    struct rcu_reader_data *p_rcu_reader = &rcu_reader;

    assert(p_rcu_reader->depth != 0);
    if (--p_rcu_reader->depth > 0) {
        return;
    }

    /* Ensure that the critical section is seen to precede the
     * store to p_rcu_reader->ctr.  Together with the following
     * smp_mb_placeholder(), this ensures writes to p_rcu_reader->ctr
     * are sequentially consistent.
     */
    atomic_store_release(&p_rcu_reader->ctr, 0);

    /* Write p_rcu_reader->ctr before reading p_rcu_reader->waiting.  */
    smp_mb_placeholder();
    if (unlikely(atomic_read(&p_rcu_reader->waiting))) {
        atomic_set(&p_rcu_reader->waiting, false);
        qemu_event_set(&rcu_gp_event);
    }
}

extern void synchronize_rcu(void);

/*
 * Reader thread registration.
 */
extern void rcu_register_thread(void);
extern void rcu_unregister_thread(void);

/*
 * Support for fork().  fork() support is enabled at startup.
 */
extern void rcu_disable_atfork(void);

struct rcu_head;
typedef void RCUCBFunc(struct rcu_head *head);

struct rcu_head {
    struct rcu_head *next;
    RCUCBFunc *func;
};

extern void call_rcu1(struct rcu_head *head, RCUCBFunc *func);

/* The operands of the minus operator must have the same type,
 * which must be the one that we specify in the cast.
 */
#define call_rcu(head, func, field)                                      \
    call_rcu1(({                                                         \
         char __attribute__((unused))                                    \
            offset_must_be_zero[-offsetof(typeof(*(head)), field)],      \
            func_type_invalid = (func) - (void (*)(typeof(head)))(func); \
         &(head)->field;                                                 \
      }),                                                                \
      (RCUCBFunc *)(func))

#define g_free_rcu(obj, field) \
    call_rcu1(({                                                         \
        char __attribute__((unused))                                     \
            offset_must_be_zero[-offsetof(typeof(*(obj)), field)];       \
        &(obj)->field;                                                   \
      }),                                                                \
      (RCUCBFunc *)g_free);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_RCU_H */
