/*
 * Support for RAM backed by mmaped host memory.
 *
 * Copyright (c) 2015 Red Hat, Inc.
 *
 * Authors:
 *  Michael S. Tsirkin <mst@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/mmap-alloc.h"
#include "qemu/host-utils.h"

#define HUGETLBFS_MAGIC       0x958458f6

#include <sys/vfs.h>

size_t qemu_fd_getpagesize(int fd)
{
    struct statfs fs;
    int ret;

    if (fd != -1) {
        do {
            ret = fstatfs(fd, &fs);
        } while (ret != 0 && errno == EINTR);

        if (ret == 0 && fs.f_type == HUGETLBFS_MAGIC) {
            return fs.f_bsize;
        }
    }
#ifdef __sparc__
    /* SPARC Linux needs greater alignment than the pagesize */
    return QEMU_VMALLOC_ALIGN;
#endif

    return getpagesize();
}

size_t qemu_mempath_getpagesize(const char *mem_path)
{
    struct statfs fs;
    int ret;

    do {
        ret = statfs(mem_path, &fs);
    } while (ret != 0 && errno == EINTR);

    if (ret != 0) {
        fprintf(stderr, "Couldn't statfs() memory path: %s\n",
                strerror(errno));
        exit(1);
    }

    if (fs.f_type == HUGETLBFS_MAGIC) {
        /* It's hugepage, return the huge page size */
        return fs.f_bsize;
    }
#ifdef __sparc__
    /* SPARC Linux needs greater alignment than the pagesize */
    return QEMU_VMALLOC_ALIGN;
#endif

    return getpagesize();
}

void *qemu_ram_mmap(int fd, size_t size, size_t align, bool shared)
{
    /*
     * Note: this always allocates at least one extra page of virtual address
     * space, even if size is already aligned.
     */
    size_t total = size + align;
    void *ptr = mmap(0, total, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    size_t offset;
    void *ptr1;

    if (ptr == MAP_FAILED) {
        return MAP_FAILED;
    }

    assert(is_power_of_2(align));
    /* Always align to host page size */
    assert(align >= getpagesize());

    offset = QEMU_ALIGN_UP((uintptr_t)ptr, align) - (uintptr_t)ptr;
    ptr1 = mmap(ptr + offset, size, PROT_READ | PROT_WRITE,
                MAP_FIXED |
                (fd == -1 ? MAP_ANONYMOUS : 0) |
                (shared ? MAP_SHARED : MAP_PRIVATE),
                fd, 0);
    if (ptr1 == MAP_FAILED) {
        munmap(ptr, total);
        return MAP_FAILED;
    }

    if (offset > 0) {
        munmap(ptr, offset);
    }

    /*
     * Leave a single PROT_NONE page allocated after the RAM block, to serve as
     * a guard page guarding against potential buffer overflows.
     */
    total -= offset;
    if (total > size + getpagesize()) {
        munmap(ptr1 + size + getpagesize(), total - size - getpagesize());
    }

    return ptr1;
}

void qemu_ram_munmap(void *ptr, size_t size)
{
    if (ptr) {
        /* Unmap both the RAM block and the guard page */
        munmap(ptr, size + getpagesize());
    }
}
