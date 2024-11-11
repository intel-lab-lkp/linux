// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <asm/unistd.h>
#include <init.h>
#include <longjmp.h>
#include <as-layout.h>
#include <os.h>
#include <sys/prctl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

void os_alarm_process(int pid)
{
/* !CONFIG_MMU doesn't send alarm signal to other processes */
#ifdef CONFIG_MMU
	kill(pid, SIGALRM);
#endif
}

void os_kill_process(int pid, int reap_child)
{
	kill(pid, SIGKILL);
	if (reap_child)
		CATCH_EINTR(waitpid(pid, NULL, __WALL));
}

/* Kill off a ptraced child by all means available.  kill it normally first,
 * then PTRACE_KILL it, then PTRACE_CONT it in case it's in a run state from
 * which it can't exit directly.
 */

void os_kill_ptraced_process(int pid, int reap_child)
{
/* !CONFIG_MMU doesn't have ptraced process */
#ifdef CONFIG_MMU
	kill(pid, SIGKILL);
	ptrace(PTRACE_KILL, pid);
	ptrace(PTRACE_CONT, pid);
	if (reap_child)
		CATCH_EINTR(waitpid(pid, NULL, __WALL));
#endif
}

/* Don't use the glibc version, which caches the result in TLS. It misses some
 * syscalls, and also breaks with clone(), which does not unshare the TLS.
 */

int os_getpid(void)
{
	return syscall(__NR_getpid);
}

int os_map_memory(void *virt, int fd, unsigned long long off, unsigned long len,
		  int r, int w, int x)
{
	void *loc;
	int prot;

	prot = (r ? PROT_READ : 0) | (w ? PROT_WRITE : 0) |
		(x ? PROT_EXEC : 0);

	loc = mmap64((void *) virt, len, prot, MAP_SHARED | MAP_FIXED |
		     (!IS_ENABLED(CONFIG_MMU) ? MAP_ANONYMOUS : 0), fd, off);
	if (loc == MAP_FAILED)
		return -errno;
	return 0;
}

int os_protect_memory(void *addr, unsigned long len, int r, int w, int x)
{
	int prot = ((r ? PROT_READ : 0) | (w ? PROT_WRITE : 0) |
		    (x ? PROT_EXEC : 0));

	if (mprotect(addr, len, prot) < 0)
		return -errno;

	return 0;
}

int os_unmap_memory(void *addr, int len)
{
	int err;

	err = munmap(addr, len);
	if (err < 0)
		return -errno;
	return 0;
}

#ifndef MADV_REMOVE
#define MADV_REMOVE KERNEL_MADV_REMOVE
#endif

int os_drop_memory(void *addr, int length)
{
	int err;

	err = madvise(addr, length, MADV_REMOVE);
	if (err < 0)
		err = -errno;
	return err;
}

int __init can_drop_memory(void)
{
	void *addr;
	int fd, ok = 0;

	printk(UM_KERN_INFO "Checking host MADV_REMOVE support...");
	fd = create_mem_file(UM_KERN_PAGE_SIZE);
	if (fd < 0) {
		printk(UM_KERN_ERR "Creating test memory file failed, "
		       "err = %d\n", -fd);
		goto out;
	}

	addr = mmap64(NULL, UM_KERN_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		printk(UM_KERN_ERR "Mapping test memory file failed, "
		       "err = %d\n", -errno);
		goto out_close;
	}

	if (madvise(addr, UM_KERN_PAGE_SIZE, MADV_REMOVE) != 0) {
		printk(UM_KERN_ERR "MADV_REMOVE failed, err = %d\n", -errno);
		goto out_unmap;
	}

	printk(UM_KERN_CONT "OK\n");
	ok = 1;

out_unmap:
	munmap(addr, UM_KERN_PAGE_SIZE);
out_close:
	close(fd);
out:
	return ok;
}

static int os_page_mincore(void *addr)
{
	char vec[2];
	int ret;

	ret = mincore(addr, UM_KERN_PAGE_SIZE, vec);
	if (ret < 0) {
		if (errno == ENOMEM || errno == EINVAL)
			return 0;
		else
			return -errno;
	}

	return vec[0] & 1;
}

int os_mincore(void *addr, unsigned long len)
{
	char *vec;
	int ret, i;

	if (len <= UM_KERN_PAGE_SIZE)
		return os_page_mincore(addr);

	vec = calloc(1, (len + UM_KERN_PAGE_SIZE - 1) / UM_KERN_PAGE_SIZE);
	if (!vec)
		return -ENOMEM;

	ret = mincore(addr, UM_KERN_PAGE_SIZE, vec);
	if (ret < 0) {
		if (errno == ENOMEM || errno == EINVAL)
			ret = 0;
		else
			ret = -errno;

		goto out;
	}

	for (i = 0; i < ((len + UM_KERN_PAGE_SIZE - 1) / UM_KERN_PAGE_SIZE); i++) {
		if (!(vec[i] & 1)) {
			ret = 0;
			goto out;
		}
	}

	ret = 1;
out:
	free(vec);
	return ret;
}

void init_new_thread_signals(void)
{
	set_handler(SIGSEGV);
	set_handler(SIGTRAP);
	set_handler(SIGFPE);
	set_handler(SIGILL);
	set_handler(SIGBUS);
	signal(SIGHUP, SIG_IGN);
	set_handler(SIGIO);
	signal(SIGWINCH, SIG_IGN);
}

void os_set_pdeathsig(void)
{
	prctl(PR_SET_PDEATHSIG, SIGKILL);
}

#ifndef CONFIG_MMU
int os_setup_seccomp(void)
{
	int err;
	unsigned long __userspace_start = uml_reserved,
		__userspace_end = high_physmem;

	struct sock_filter filter[] = {
		/* if (IP_high > __userspace_end) allow; */
		BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
			 offsetof(struct seccomp_data, instruction_pointer) + 4),
		BPF_JUMP(BPF_JMP + BPF_JGT + BPF_K, __userspace_end >> 32,
			 /*true-skip=*/0, /*false-skip=*/1),
		BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

		/* if (IP_high == __userspace_end && IP_low >= __userspace_end) allow; */
		BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
			 offsetof(struct seccomp_data, instruction_pointer) + 4),
		BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __userspace_end >> 32,
			 /*true-skip=*/0, /*false-skip=*/3),
		BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
			 offsetof(struct seccomp_data, instruction_pointer)),
		BPF_JUMP(BPF_JMP + BPF_JGE + BPF_K, __userspace_end,
			 /*true-skip=*/0, /*false-skip=*/1),
		BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

		/* if (IP_high < __userspace_start) allow; */
		BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
			 offsetof(struct seccomp_data, instruction_pointer) + 4),
		BPF_JUMP(BPF_JMP + BPF_JGE + BPF_K, __userspace_start >> 32,
			 /*true-skip=*/1, /*false-skip=*/0),
		BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

		/* if (IP_high == __userspace_start && IP_low < __userspace_start) allow; */
		BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
			 offsetof(struct seccomp_data, instruction_pointer) + 4),
		BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __userspace_start >> 32,
			 /*true-skip=*/0, /*false-skip=*/3),
		BPF_STMT(BPF_LD + BPF_W + BPF_ABS,
			 offsetof(struct seccomp_data, instruction_pointer)),
		BPF_JUMP(BPF_JMP + BPF_JGE + BPF_K, __userspace_start,
			 /*true-skip=*/1, /*false-skip=*/0),
		BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

		/* other address; trap  */
		BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_TRAP),
	};
	struct sock_fprog prog = {
		.len = ARRAY_SIZE(filter),
		.filter = filter,
	};

	err = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	if (err)
		os_warn("PR_SET_NO_NEW_PRIVS (err=%d, ernro=%d)\n",
		       err, errno);

	err = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
		      SECCOMP_FILTER_FLAG_TSYNC, &prog);
	if (err) {
		os_warn("SECCOMP_SET_MODE_FILTER (err=%d, ernro=%d)\n",
		       err, errno);
		exit(-1);
	}

	os_info("seccomp: filter syscalls in the range: 0x%lx-0x%lx\n",
		__userspace_start, __userspace_end);

	return 0;
}
#endif
