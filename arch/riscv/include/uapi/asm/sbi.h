/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (C) 2025 Lanxincomputing Corporation or its affiliates.
 *
 * Authors:
 *     BillXiang <xiangwencheng@lanxincomputing.com>
 */

#ifndef _UAPI_ASM_RISCV_SBI_H
#define _UAPI_ASM_RISCV_SBI_H


enum SBI_EXT_ID {
	/*
	* The CONSOLE_GETCHAR/CONSOLE_PUTCHAR SBI calls cannot be
	* handled in kernel so they will be forwarded to userspace by kvm.
	*/
	SBI_EXT_0_1_CONSOLE_PUTCHAR = 0x1,
	SBI_EXT_0_1_CONSOLE_GETCHAR = 0x2,
	/*
	* Both SBI experimental and vendor extensions are
	* unconditionally forwarded to userspace by kvm.
	*/
	/* Experimentals extensions must lie within this range */
	SBI_EXT_EXPERIMENTAL_START = 0x08000000,
	SBI_EXT_EXPERIMENTAL_END = 0x08FFFFFF,

	/* Vendor extensions must lie within this range */
	SBI_EXT_VENDOR_START = 0x09000000,
	SBI_EXT_VENDOR_END = 0x09FFFFFF,
};

/*
* The SBI debug console functions are unconditionally
* forwarded to the userspace by kvm.
*/
enum sbi_ext_dbcn_fid {
	SBI_EXT_DBCN_CONSOLE_WRITE = 0,
	SBI_EXT_DBCN_CONSOLE_READ = 1,
	SBI_EXT_DBCN_CONSOLE_WRITE_BYTE = 2,
};

#endif /* _UAPI_ASM_RISCV_SBI_H */