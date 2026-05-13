// SPDX-License-Identifier: GPL-2.0
#include "util/jitdump.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <internal/lib.h> // page_size

#include "../tests.h"

#define CHK_BYTE 0x5a

static inline uint64_t get_timestamp(void)
{
#if defined(__x86_64__) || defined(__i386__)
	unsigned int low, high;

	asm volatile("rdtsc" : "=a"(low), "=d"(high));

	return low | ((uint64_t)high) << 32;
#else
	struct timespec ts;
	int ret;

	ret = clock_gettime(CLOCK_MONOTONIC, &ts);
	if (ret)
		return 0;

	return ((uint64_t)ts.tv_sec * 1000000000) + ts.tv_nsec;
#endif
}

static FILE *open_jitdump(void)
{
	struct jitheader header = {
		.magic = JITHEADER_MAGIC,
		.version = JITHEADER_VERSION,
		.total_size = sizeof(header),
		.pid = getpid(),
		.timestamp = get_timestamp(),
		.flags = JITDUMP_FLAGS_ARCH_TIMESTAMP,
	};
	char filename[256];
	FILE *f;
	void *m;

	snprintf(filename, sizeof(filename), "jit-%d.dump", getpid());
	f = fopen(filename, "w+");
	if (!f) {
		pr_err("Failed to open jitdump '%s'\n", filename);
		return NULL;
	}
	/* Create an MMAP event for the jitdump file. That is how perf tool finds it. */
	m = mmap(0, page_size, PROT_READ | PROT_EXEC, MAP_PRIVATE, fileno(f), 0);
	if (m == MAP_FAILED)
		pr_err("Error creating jitdump MMAP event\n");
	else
		munmap(m, page_size);

	if (fwrite(&header, sizeof(header), 1, f) != 1) {
		pr_err("Error writing jitdump header\n");
		fclose(f);
		return NULL;
	}
	return f;
}

static int write_jitdump(FILE *f, void *addr, const void *dat, size_t sz, uint64_t *idx)
{
	struct jr_code_load rec = {
		.p.id = JIT_CODE_LOAD,
		.p.total_size = sizeof(rec) + sz,
		.p.timestamp = get_timestamp(),
		.pid = getpid(),
		.tid = getpid(),
		.vma = (unsigned long)addr,
		.code_addr = (unsigned long)addr,
		.code_size = sz,
		.code_index = ++*idx,
	};

	if (fwrite(&rec, sizeof(rec), 1, f) != 1 || fwrite(dat, sz, 1, f) != 1)
		return -1;
	return 0;
}

static void close_jitdump(FILE *f)
{
	fclose(f);
}

static int jitdump(int argc __maybe_unused, const char **argv __maybe_unused)
{
#if defined(__x86_64__) || defined(__i386__)
	/* Code to execute: mov CHK_BYTE, %eax ; ret */
	uint8_t dat[] = { 0xb8, CHK_BYTE, 0x00, 0x00, 0x00, 0xc3 };
#elif defined(__aarch64__)
	/* Code to execute: mov w0, #CHK_BYTE ; ret */
	uint32_t dat[] = { 0x52800000 | (CHK_BYTE << 5), 0xd65f03c0 };
#elif defined(__riscv)
	/* Code to execute: li a0, CHK_BYTE ; ret */
	uint32_t dat[] = { ((CHK_BYTE & 0xfff) << 20) | 0x513, 0x00008067 };
#elif defined(__powerpc__)
	/* Code to execute: li r3, CHK_BYTE ; blr */
	uint32_t dat[] = { 0x38600000 | (CHK_BYTE & 0xffff), 0x4e800020 };
#elif defined(__s390x__)
	/* Code to execute: lhi %r2, CHK_BYTE ; br %r14 */
	uint8_t dat[] = { 0xa7, 0x28, (CHK_BYTE >> 8) & 0xff, CHK_BYTE & 0xff, 0x07, 0xfe };
#elif defined(__arm__)
	/* Code to execute: mov r0, #CHK_BYTE ; bx lr */
	uint32_t dat[] = { 0xe3a00000 | (CHK_BYTE & 0xff), 0xe12fff1e };
#elif defined(__mips__)
	/* Code to execute: addiu $v0, $zero, CHK_BYTE ; jr $ra ; nop */
	uint32_t dat[] = { 0x24020000 | (CHK_BYTE & 0xffff), 0x03e00008, 0x00000000 };
#elif defined(__loongarch__)
	/* Code to execute: addi.w $a0, $zero, CHK_BYTE ; jirl $zero, $ra, 0 */
	uint32_t dat[] = { 0x02800004 | ((CHK_BYTE & 0xfff) << 10), 0x4c000020 };
#else
	uint32_t dat[0];
#endif
	void *addr;
	FILE *f;
	uint64_t idx = 0;
	int ret = 1;

	/* Get a memory page to store executable code. */
	addr = mmap(0, page_size, PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (addr == MAP_FAILED) {
		pr_err("Failed to map 1 -wx page\n");
		return 1;
	}

	f = open_jitdump();
	if (!f) {
		pr_err("Failed to open jitdump\n");
		munmap(addr, page_size);
		return 1;
	}
	/* Copy executable code to executable memory page. */
	memcpy(addr, dat, sizeof(dat));
	/* Record it in the jitdump file */
	if (write_jitdump(f, addr, dat, sizeof(dat), &idx) == 0) {
		if (sizeof(dat) > 0) {
			int (*fn)(void) = addr;

			/* Call the function. */
			ret = fn() - CHK_BYTE;
		} else {
			pr_err("jitdump workload not supported on this architecture\n");
			ret = 1;
		}
	}
	close_jitdump(f);
	munmap(addr, page_size);
	return ret;
}

DEFINE_WORKLOAD(jitdump);
