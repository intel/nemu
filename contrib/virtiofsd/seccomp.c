/*
 * Seccomp sandboxing for virtiofsd
 *
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdlib.h>
#include <err.h>
#include <errno.h>
#include <seccomp.h>
#include <glib.h>
#include "seccomp.h"

static const int syscall_whitelist[] = {
	/* TODO ireg sem*() syscalls */
	SCMP_SYS(brk),
	SCMP_SYS(clone),
	SCMP_SYS(close),
	SCMP_SYS(copy_file_range),
	SCMP_SYS(dup),
	SCMP_SYS(eventfd2),
	SCMP_SYS(exit),
	SCMP_SYS(exit_group),
	SCMP_SYS(fallocate),
	SCMP_SYS(fchmodat),
	SCMP_SYS(fchownat),
	SCMP_SYS(fcntl),
	SCMP_SYS(fdatasync),
	SCMP_SYS(fgetxattr),
	SCMP_SYS(flistxattr),
	SCMP_SYS(flock),
	SCMP_SYS(fremovexattr),
	SCMP_SYS(fsetxattr),
	SCMP_SYS(fstat),
	SCMP_SYS(fstatfs),
	SCMP_SYS(fsync),
	SCMP_SYS(ftruncate),
	SCMP_SYS(futex),
	SCMP_SYS(getdents),
	SCMP_SYS(getdents64),
	SCMP_SYS(getegid),
	SCMP_SYS(geteuid),
	SCMP_SYS(linkat),
	SCMP_SYS(lseek),
	SCMP_SYS(madvise),
	SCMP_SYS(mkdirat),
	SCMP_SYS(mknodat),
	SCMP_SYS(mmap),
	SCMP_SYS(mprotect),
	SCMP_SYS(mremap),
	SCMP_SYS(munmap),
	SCMP_SYS(newfstatat),
	SCMP_SYS(open),
	SCMP_SYS(openat),
	SCMP_SYS(ppoll),
	SCMP_SYS(preadv),
	SCMP_SYS(pwrite64),
	SCMP_SYS(read),
	SCMP_SYS(readlinkat),
	SCMP_SYS(recvmsg),
	SCMP_SYS(renameat),
	SCMP_SYS(renameat2),
	SCMP_SYS(rt_sigaction),
	SCMP_SYS(rt_sigreturn),
	SCMP_SYS(sendmsg),
	SCMP_SYS(setresgid),
	SCMP_SYS(setresuid),
	SCMP_SYS(set_robust_list),
	SCMP_SYS(symlinkat),
	SCMP_SYS(unlinkat),
	SCMP_SYS(utimensat),
	SCMP_SYS(write),
};

void setup_seccomp(void)
{
	scmp_filter_ctx ctx;
	size_t i;

#ifdef SCMP_ACT_KILL_PROCESS
 	ctx = seccomp_init(SCMP_ACT_KILL_PROCESS);
#else
	ctx = seccomp_init(SCMP_ACT_KILL);
#endif
	if (!ctx) {
		err(1, "seccomp_init()");
	}

	if (seccomp_attr_set(ctx, SCMP_FLTATR_CTL_TSYNC, 1) != 0) {
		err(1, "seccomp_attr_set(ctx, SCMP_FLTATTR_CTL_TSYNC, 1)");
	}

	for (i = 0; i < G_N_ELEMENTS(syscall_whitelist); i++) {
		if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW,
				     syscall_whitelist[i], 0) != 0) {
			err(1, "seccomp_rule_add syscall %d",
			    syscall_whitelist[i]);
		}
	}

	/* libvhost-user calls this for post-copy migration, we don't need it */
	if (seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(userfaultfd), 0) != 0) {
		err(1, "seccomp_rule_add userfaultfd");
	}

	if (seccomp_load(ctx) < 0) {
		err(1, "seccomp_load()");
	}

	seccomp_release(ctx);
}
