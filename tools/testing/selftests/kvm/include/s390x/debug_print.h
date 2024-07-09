/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Definition for kernel virtual machines on s390x
 *
 * Copyright IBM Corp. 2024
 *
 * Authors:
 *  Christoph Schlameuss <schlameuss@linux.ibm.com>
 */

#ifndef SELFTEST_KVM_DEBUG_PRINT_H
#define SELFTEST_KVM_DEBUG_PRINT_H

#include "kvm_util.h"
#include "sie.h"

static inline void print_hex_bytes(const char *name, u64 page, size_t len)
{
	pr_debug("%s (%p)\t\t8-0x08  12-0x0c  16-0x10  20-0x14  24-0x18  28-0x1c",
		 name, (void *)page);
	for (u8 pp_row = 0; pp_row < (len / 32); pp_row++) {
		pr_debug("\n %3d 0x%.3x ", pp_row * 32, pp_row * 32);
		for (u8 pp_block = 0; pp_block < 8; pp_block++)
			pr_debug(" %8x", *(((u32 *)page) + 8 * pp_row + pp_block));
	}
	pr_debug("\n");
}

static inline void print_hex(const char *name, u64 page)
{
	print_hex_bytes(name, page, 512);
}

static inline void print_psw(struct kvm_run *run, struct kvm_s390_sie_block *sie_block)
{
	pr_debug("flags:0x%x psw:0x%.16llx:0x%.16llx exit:%u %s\n",
		 run->flags,
		 run->psw_mask, run->psw_addr,
		 run->exit_reason, exit_reason_str(run->exit_reason));
	pr_debug("sie_block psw:0x%.16llx:0x%.16llx\n",
		 sie_block->psw_mask, sie_block->psw_addr);
}

static inline void print_run(struct kvm_run *run, struct kvm_s390_sie_block *sie_block)
{
	print_hex("run", (u64)run);
	print_hex("sie_block", (u64)sie_block);
	print_psw(run, sie_block);
}

static inline void print_regs(int vcpu_fd)
{
	struct kvm_sregs sregs = {};
	struct kvm_regs regs = {};
	int i;

	if (ioctl(vcpu_fd, KVM_GET_REGS, &regs) != 0)
		TEST_FAIL("get regs failed, %s", strerror(errno));
	pr_debug("gprs:\n");
	for (i = 0; i < 16; i += 4)
		pr_debug("0x%.16llx 0x%.16llx 0x%.16llx 0x%.16llx\n",
			 regs.gprs[i], regs.gprs[i + 1],
			 regs.gprs[i + 2], regs.gprs[i + 3]);
	if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) != 0)
		TEST_FAIL("get sregs failed, %s", strerror(errno));
	pr_debug("acrs:\n");
	for (i = 0; i < 16; i += 4)
		pr_debug("0x%.8x 0x%.8x 0x%.8x 0x%.8x\n",
			 sregs.acrs[i], sregs.acrs[i + 1],
			 sregs.acrs[i + 2], sregs.acrs[i + 3]);
	pr_debug("crs:\n");
	for (i = 0; i < 16; i += 4)
		pr_debug("0x%.16llx 0x%.16llx 0x%.16llx 0x%.16llx\n",
			 sregs.crs[i], sregs.crs[i + 1],
			 sregs.crs[i + 2], sregs.crs[i + 3]);
}

#endif /* SELFTEST_KVM_DEBUG_PRINT_H */
