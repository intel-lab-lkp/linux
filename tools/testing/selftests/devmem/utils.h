/* SPDX-License-Identifier: GPL-2.0+ */
/* devmem test utils.h
 *
 * Copyright (C) 2025 Red Hat, Inc. All Rights Reserved.
 * Written by Alessandro Carminati (acarmina@redhat.com)
 */

#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define BOUNCE_BUF_SIZE			64
/*
 * Test Case Flags:
 * F_ARCH_ALL: Test valid on all HW Architectures.
 * F_ARCH_X86: Test valid on x86 only.
 * F_ARCH_ARM: Test valid on ARM only.
 * F_ARCH_PPC: Test valid on PowerPC only.
 * F_ARCH_MIPS: Test valid on MIPS only.
 * F_ARCH_S390: Test valid on S390 only.
 * F_ARCH_RISCV: Test valid on RISC-V only.
 *
 * F_BITS_ALL: Test valid on both 32b and 64b systems.
 * F_BITS_B64: Test valid on 64b systems only.
 * F_BITS_B32: Test valid on 32b systems only.
 *
 * F_MISC_FATAL: a test failure stops the execution of any other test.
 * F_MISC_STRICT_DEVMEM_REQ: the test requires STRICT_DEVMEM to be defined
 *                           in the Kernel.
 * F_MISC_STRICT_DEVMEM_PRV: the test retrieves the status of STRICT_DEVMEM
 *                           (whether it is defined or not in the Kernel).
 * F_MISC_INIT_PRV: the test verify the system to be in a proper init state
 *                  for subsequent tests to run.
 * F_MISC_INIT_REQ: the test requires a proper init state as retrieved by
 *                  F_MISC_INIT_PRV.
 * F_MISC_DONT_CARE: the test is not part of the test plan, it is just
 *                   auxiliary code that determine how to run other tests.
 * F_MISC_WARN_ON_SUCCESS: This flags is applicable to negative tests. I.e.
 *                         it raises a Warning if an operation succeeds when
 *                         it is expected to fail.
 * F_MISC_WARN_ON_FAILURE: This flags is applicable to positive tests. I.e.
 *                         it raises a Warning if an operation fails when it
 *                         is expected to succeed.
 */
#define F_ARCH_ALL			1
#define F_ARCH_X86			(1 << 1)
#define F_ARCH_ARM			(1 << 2)
#define F_ARCH_PPC			(1 << 3)
#define F_ARCH_MIPS			(1 << 4)
#define F_ARCH_S390			(1 << 5)
#define F_ARCH_RISCV			(1 << 6)

#define F_BITS_ALL			(1 << 7)
#define F_BITS_B64			(1 << 8)
#define F_BITS_B32			(1 << 9)

#define F_MISC_FATAL			(1 << 10)
#define F_MISC_STRICT_DEVMEM_REQ	(1 << 11)
#define F_MISC_STRICT_DEVMEM_PRV	(1 << 12)
#define F_MISC_INIT_PRV			(1 << 13)
#define F_MISC_INIT_REQ			(1 << 14)
#define F_MISC_DONT_CARE		(1 << 15)
#define F_MISC_WARN_ON_SUCCESS		(1 << 16)
#define F_MISC_WARN_ON_FAILURE		(1 << 17)

enum {
	TEST_DENIED,
	TEST_INCOHERENT,
	TEST_ALLOWED
};

struct test_context {
	struct ram_map	*map;
	char		*srcbuf;
	char		*dstbuf;
	uintptr_t	tst_addr;
	int		fd;
	bool		verbose;
	bool		strict_devmem_state;
	bool		devmem_init_state;
};

/*
 * struct char_mem_test - test case structure for testing /drivers/char/mem.c
 * @name: name of the test case.
 * @fn: test callback implementing the test case.
 * @descr: test case descriptor; it must be formatted as
 *         "short description"-"function-name"-"FE<i>"
 *         where
 *         "short description" describe what the test case does,
 *         "function-name" is the name of the tested function in
 *         /drivers/char/mem.c,
 *         "FE<i>" is the list of tested Function's Expectations from the
 *         kernel-doc header associated with "function-name".
 * @flags: test case applicable flags (see list above).
 */
struct char_mem_test {
	char		*name;
	int		(*fn)(struct test_context *t);
	char		*descr;
	uint64_t	flags;
};

uint64_t virt_to_phys(void *virt_addr);
int try_read_inplace(int fd, int scnt, void *sbuf);
int try_read_dev_mem(int fd, uint64_t addr, int scnt, void *sbuf);
int try_write_dev_mem(int fd, uint64_t addr, int scnt, void *sbuf);
int fill_random_chars(char *buf, int cnt);
bool is_zero(const void *p, size_t cnt);
void print_hex(const void *p, size_t cnt);
int test_needed(struct test_context *t, struct char_mem_test *current);
void *malloc_pb(size_t size);
void free_pb(void *ptr);

#endif

