/* SPDX-License-Identifier: GPL-2.0 */
#include <syscall.h>

/*
 * Define sys_xyx to call syscall directly.
 * This is needed because we want to avoid calling glibc and
 * test syscall directly.
 * The only exception is mmap, which _NR_mmap2 is not defined for
 * some ARM architecture.
 */
static inline int sys_mseal(void *start, size_t len, unsigned long flags)
{
	int sret;

	errno = 0;
	sret = syscall(__NR_mseal, start, len, flags);
	return sret;
}

static inline int sys_mprotect(void *ptr, size_t size, unsigned long prot)
{
	int sret;

	errno = 0;
	sret = syscall(__NR_mprotect, ptr, size, prot);
	return sret;
}

static inline int sys_mprotect_pkey(void *ptr, size_t size,
	unsigned long orig_prot, unsigned long pkey)
{
	int sret;

	errno = 0;
	sret = syscall(__NR_pkey_mprotect, ptr, size, orig_prot, pkey);
	return sret;
}

static inline int sys_munmap(void *ptr, size_t size)
{
	int sret;

	errno = 0;
	sret = syscall(__NR_munmap, ptr, size);
	return sret;
}

static inline int sys_madvise(void *start, size_t len, int types)
{
	int sret;

	errno = 0;
	sret = syscall(__NR_madvise, start, len, types);
	return sret;
}

static inline void *sys_mremap(void *addr, size_t old_len, size_t new_len,
	unsigned long flags, void *new_addr)
{
	void *sret;

	errno = 0;
	sret = (void *) syscall(__NR_mremap, addr, old_len, new_len, flags, new_addr);
	return sret;
}

/*
 * Parsing /proc/self/maps to get VMA's size and prot bit.
 */
static unsigned long get_vma_size(void *addr, int *prot)
{
	FILE *maps;
	char line[256];
	int size = 0;
	uintptr_t  addr_start, addr_end;
	char protstr[5];
	*prot = 0;

	maps = fopen("/proc/self/maps", "r");
	if (!maps)
		return 0;

	while (fgets(line, sizeof(line), maps)) {
		if (sscanf(line, "%lx-%lx %4s", &addr_start, &addr_end, protstr) == 3) {
			if (addr_start == (uintptr_t) addr) {
				size = addr_end - addr_start;
				if (protstr[0] == 'r')
					*prot |= PROT_READ;
				if (protstr[1] == 'w')
					*prot |= PROT_WRITE;
				if (protstr[2] == 'x')
					*prot |= PROT_EXEC;
				break;
			}
		}
	}
	fclose(maps);
	return size;
}

static inline bool mseal_supported(void)
{
	int ret;
	void *ptr;
	unsigned long page_size = getpagesize();

	ptr = mmap(NULL, page_size, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (ptr == MAP_FAILED)
		return false;

	ret = sys_mseal(ptr, page_size, 0);
	if (ret < 0)
		return false;

	return true;
}
