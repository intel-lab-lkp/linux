// SPDX-License-Identifier: GPL-2.0+
/* devmem test utils.c
 *
 * Copyright (C) 2025 Red Hat, Inc. All Rights Reserved.
 * Written by Alessandro Carminati (acarmina@redhat.com)
 */

#define _FILE_OFFSET_BITS 64
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "utils.h"
#include "debug.h"


static inline uint64_t get_page_size(void)
{
	return (uint64_t)sysconf(_SC_PAGE_SIZE);
}

uint64_t virt_to_phys(void *virt_addr)
{
	uint64_t virt_pfn, page_size, phys_addr, pfn;
	uintptr_t virt = (uintptr_t)virt_addr;
	ssize_t bytes_read;
	uint64_t entry = 0;
	off_t offset;
	int fd;

	page_size = get_page_size();
	virt_pfn = virt / page_size;
	deb_printf("page_size=%d, virt_pfn=%lu\n", page_size, virt_pfn);

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) {
		deb_printf("Error opening /proc/self/pagemap: %s\n",
		  strerror(errno));
		return 0;
	}

	offset = (off_t)(virt_pfn * sizeof(uint64_t));
	deb_printf("lseek(%d, 0x%llx, SEEK_SET)\n", fd, offset);
	if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
		deb_printf("Error seeking pagemap: %s\n", strerror(errno));
		close(fd);
		return 0;
	}

	bytes_read = read(fd, &entry, sizeof(entry));
	close(fd);
	if (bytes_read != sizeof(entry)) {
		deb_printf("Error reading pagemap: %s\n", strerror(errno));
		return 0;
	}

	if (!(entry & (1ULL << 63))) {
		deb_printf("Page not present in RAM (maybe swapped out).\n");
		return 0;
	}

	pfn = entry & ((1ULL << 55) - 1);
	deb_printf("entry=%llx, pfn=%llx\n", entry, pfn);
	if (pfn == 0) {
		deb_printf("PFN is 0 - invalid mapping.\n");
		return 0;
	}

	phys_addr = (pfn * page_size) + (virt % page_size);
	deb_printf("phys_addr=%llx\n", phys_addr);
	return phys_addr;
}

int try_read_inplace(int fd, int scnt, void *sbuf)
{
	ssize_t r;

	r = read(fd, sbuf, scnt);
	deb_printf("read(%d, %p, %d)=%d(%d)\n", fd, sbuf, scnt, r, -errno);
	if (r < 0)
		return -errno;

	return (int)r;
}

int try_read_dev_mem(int fd, uint64_t addr, int scnt, void *sbuf)
{
	int space;
	ssize_t r;
	void *buf;
	int cnt;

	buf = sbuf ? sbuf : &space;
	cnt = sbuf ? scnt : sizeof(space);
	deb_printf("buf = %p, cnt = %d\n", buf, cnt);
	if (lseek(fd, (off_t)addr, SEEK_SET) == (off_t)-1)
		return -errno;

	deb_printf("lseek(%d, %llx, SEEK_SET)=%d\n", fd, addr, -errno);

	r = read(fd, buf, cnt);
	deb_printf("read(%d, %p, %d)=%d(%d)\n", fd, buf, cnt, r, -errno);
	if (r < 0)
		return -errno;

	return (int)r;
}

int try_write_dev_mem(int fd, uint64_t addr, int scnt, void *sbuf)
{
	int space;
	ssize_t r;
	void *buf;
	int cnt;

	buf = sbuf ? sbuf : &space;
	cnt = sbuf ? scnt : sizeof(space);
	deb_printf("buf = %p, cnt = %d\n", buf, cnt);
	if (lseek(fd, (off_t)addr, SEEK_SET) == (off_t)-1)
		return -errno;

	deb_printf("lseek(%d, %llx, SEEK_SET)=%d\n", fd, addr, -errno);

	r = write(fd, buf, cnt);
	deb_printf("write(%d, %p, %d)=%d(%d)\n", fd, buf, cnt, r, -errno);
	if (r < 0)
		return -errno;

	return (int)r;
}

int fill_random_chars(char *buf, int cnt)
{
	int bytes_read, fd;
	ssize_t res;

	if (!buf || cnt <= 0) {
		errno = EINVAL;
		return -1;
	}

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0) {
		perror("open /dev/urandom");
		return -1;
	}

	bytes_read = 0;
	while (bytes_read < cnt) {
		res = read(fd, buf + bytes_read, cnt - bytes_read);
		if (res < 0) {
			if (errno == EINTR)
				continue;
			perror("read /dev/urandom");
			close(fd);
			return -1;
		}
		bytes_read += res;
	}
	close(fd);

	return 0;
}

bool is_zero(const void *p, size_t cnt)
{
	const char *byte_ptr = (const char *)p;

	for (size_t i = 0; i < cnt; ++i) {
		if (byte_ptr[i] != 0)
			return false;
	}
	return true;
}

void print_hex(const void *p, size_t cnt)
{
	const unsigned char *bytes = (const unsigned char *)p;
	int remainder;
	size_t i;

	for (i = 0; i < cnt; i++) {
		if (i % 16 == 0) {
			if (i > 0)
				printf("\n");

			printf("%08lX: ", (unsigned long)(bytes + i));
		}
		printf("%02X ", bytes[i]);
	}

	remainder = cnt % 16;
	if (remainder != 0) {
		for (int j = 0; j < 16 - remainder; j++)
			printf("   ");
	}

	printf("\n");
}

static bool machine_is_compatible(unsigned int flags)
{
	unsigned int current_arch_flag = 0;
	unsigned int current_bits_flag = 0;

#if defined(__x86_64__) || defined(__i386__)
	current_arch_flag = F_ARCH_X86;
#elif defined(__arm__) || defined(__aarch64__)
	current_arch_flag = F_ARCH_ARM;
#elif defined(__PPC__) || defined(__powerpc__)
	current_arch_flag = F_ARCH_PPC;
#elif defined(__mips__)
	current_arch_flag = F_ARCH_MIPS;
#elif defined(__s390__)
	current_arch_flag = F_ARCH_S390;
#elif defined(__riscv)
	current_arch_flag = F_ARCH_RISCV;
#else
	current_arch_flag = 0;
#endif

	if (sizeof(void *) == 8)
		current_bits_flag = F_BITS_B64;
	else
		current_bits_flag = F_BITS_B32;

	bool arch_matches = (flags & F_ARCH_ALL) || (flags & current_arch_flag);

	bool bits_matches = (flags & F_BITS_ALL) || (flags & current_bits_flag);

	return arch_matches && bits_matches;
}

static void print_flags(uint32_t flags)
{
	printf("Flags: 0x%08X ->", flags);

	// Architecture flags
	printf(" Architecture: ");
	if (flags & F_ARCH_ALL)
		printf("ALL ");

	if (flags & F_ARCH_X86)
		printf("X86 ");

	if (flags & F_ARCH_ARM)
		printf("ARM ");

	if (flags & F_ARCH_PPC)
		printf("PPC ");

	if (flags & F_ARCH_MIPS)
		printf("MIPS ");

	if (flags & F_ARCH_S390)
		printf("S390 ");

	if (flags & F_ARCH_RISCV)
		printf("RISC-V ");

	// Bitness flags
	printf(" Bitness: ");
	if (flags & F_BITS_ALL)
		printf("ALL ");

	if (flags & F_BITS_B64)
		printf("64-bit ");

	if (flags & F_BITS_B32)
		printf("32-bit ");

	// Miscellaneous flags
	printf(" Miscellaneous:");
	if (flags & F_MISC_FATAL)
		printf("	- F_MISC_FATAL: true");

	if (flags & F_MISC_STRICT_DEVMEM_REQ)
		printf("	- F_MISC_STRICT_DEVMEM_REQ: true");

	if (flags & F_MISC_STRICT_DEVMEM_PRV)
		printf("	- F_MISC_STRICT_DEVMEM_PRV: true");

	if (flags & F_MISC_INIT_PRV)
		printf("	- F_MISC_INIT_PRV: true");

	if (flags & F_MISC_INIT_REQ)
		printf("	- F_MISC_INIT_REQ: true");

	printf("\n");
}

static void print_context(struct test_context *t)
{
	char *c;

	c = "NO";
	if (t->devmem_init_state)
		c = "yes";
	printf("system state: init=%s, ", c);
	c = "NO";
	if (t->strict_devmem_state)
		c = "yes";
	printf("strict_devmem=%s\n", c);
}

int test_needed(struct test_context *t,
			     struct char_mem_test *current)
{
	if (t->verbose) {
		print_context(t);
		print_flags(current->flags);
	}

	if (!(t->devmem_init_state) && !(current->flags & F_MISC_INIT_PRV)) {
		deb_printf("Not initialized and test does not provide initialization\n");
		return TEST_DENIED;// Not initialized and not provide init
	}
	if ((t->devmem_init_state) && (current->flags & F_MISC_INIT_PRV)) {
		deb_printf("can not initialize again\n");
		return TEST_INCOHERENT;	// can not initialize again
	}
	if (!(t->devmem_init_state) && (current->flags & F_MISC_INIT_PRV)) {
		deb_printf("initializing: test allowed!\n");
		return TEST_ALLOWED;	// initializing: test allowed!
	}
	if (!(t->devmem_init_state)) {
		deb_printf("not initialized, can not proceed\n");
		return TEST_DENIED;	// not initialized, can not proceed
	}
	if (!(machine_is_compatible(current->flags))) {
		deb_printf("not for this architecture\n");
		return TEST_DENIED;	// not for this architecture
	}
	if (((t->strict_devmem_state) || (current->flags &
	    F_MISC_STRICT_DEVMEM_REQ)) && !((t->strict_devmem_state) &&
	    (current->flags & F_MISC_STRICT_DEVMEM_REQ))) {
		deb_printf("strict_devmem requirement and offering do not meet\n");
		return TEST_DENIED;// strict_devmem requirement
	}
	deb_printf("test allowed!\n");
	return TEST_ALLOWED;
}

void *malloc_pb(size_t size)
{
	if (size == 0 || size > getpagesize()) {
		fprintf(stderr, "size must be greater than 0 and less than or equal to one page.\n");
		return NULL;
	}

	void *ptr = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (ptr == MAP_FAILED) {
		perror("mmap failed");
		return NULL;
	}

	return ptr;
}

void free_pb(void *ptr)
{
	if (ptr == NULL)
		return;

	if (munmap(ptr, getpagesize()) == -1)
		perror("munmap failed");

}
