/*
 * common defines for all CPUs
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#ifndef CPU_DEFS_H
#define CPU_DEFS_H

#ifndef NEED_CPU_H
#error cpu.h included from common code
#endif

#include "qemu/host-utils.h"
#include "qemu/queue.h"
#include "exec/hwaddr.h"
#include "exec/memattrs.h"

#ifndef TARGET_LONG_BITS
#error TARGET_LONG_BITS must be defined before including this header
#endif

#define TARGET_LONG_SIZE (TARGET_LONG_BITS / 8)

/* target_ulong is the type of a virtual address */
#if TARGET_LONG_SIZE == 4
typedef int32_t target_long;
typedef uint32_t target_ulong;
#define TARGET_FMT_lx "%08x"
#define TARGET_FMT_ld "%d"
#define TARGET_FMT_lu "%u"
#elif TARGET_LONG_SIZE == 8
typedef int64_t target_long;
typedef uint64_t target_ulong;
#define TARGET_FMT_lx "%016" PRIx64
#define TARGET_FMT_ld "%" PRId64
#define TARGET_FMT_lu "%" PRIu64
#else
#error TARGET_LONG_SIZE undefined
#endif


#define CPU_COMMON_TLB



#define CPU_COMMON                                                      \
    /* soft mmu support */                                              \
    CPU_COMMON_TLB                                                      \

#endif
