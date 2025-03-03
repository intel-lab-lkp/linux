/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (C) 2021-2024 Advanced Micro Devices, Inc.
 */
#ifndef _AMD_APML_H_
#define _AMD_APML_H_

#include <linux/types.h>

enum apml_protocol {
	APML_CPUID	= 0x1000,
	APML_MCA_MSR,
};

/* These are byte indexes into data_in and data_out arrays */
#define AMD_SBI_RD_WR_DATA_INDEX	0
#define AMD_SBI_REG_OFF_INDEX		0
#define AMD_SBI_REG_VAL_INDEX		4
#define AMD_SBI_RD_FLAG_INDEX		7
#define AMD_SBI_THREAD_LOW_INDEX	4
#define AMD_SBI_THREAD_HI_INDEX		5
#define AMD_SBI_EXT_FUNC_INDEX		6

#define AMD_SBI_MB_DATA_SIZE		4

struct apml_message {
	/* message ids:
	 * Mailbox Messages:	0x0 ... 0x999
	 * APML_CPUID:		0x1000
	 * APML_MCA_MSR:	0x1001
	 */
	__u32 cmd;

	/*
	 * 8 bit data for reg read,
	 * 32 bit data in case of mailbox,
	 * up to 64 bit in case of cpuid and mca msr
	 */
	union {
		__u64 cpu_msr_out;
		__u32 mb_out[2];
		__u8 reg_out[8];
	} data_out;

	/*
	 * [0]...[3] mailbox 32bit input
	 *	     cpuid & mca msr,
	 * [4][5] cpuid & mca msr: thread
	 * [4] rmi reg wr: value
	 * [6] cpuid: ext function & read eax/ebx or ecx/edx
	 *	[7:0] -> bits [7:4] -> ext function &
	 *	bit [0] read eax/ebx or ecx/edx
	 * [7] read/write functionality
	 */
	union {
		__u64 cpu_msr_in;
		__u32 mb_in[2];
		__u8 reg_in[8];
	} data_in;
	/*
	 * Status code is returned in case of CPUID/MCA access
	 * Error code is returned in case of soft mailbox
	 */
	__u32 fw_ret_code;
} __attribute__((packed));

/*
 * AMD sideband interface base IOCTL
 */
#define SB_BASE_IOCTL_NR	0xF9

/**
 * DOC: SBRMI_IOCTL_CMD
 *
 * @Parameters
 *
 * @struct apml_message
 *	Pointer to the &struct apml_message that will contain the protocol
 *	information
 *
 * @Description
 * IOCTL command for APML messages using generic _IOWR
 * The IOCTL provides userspace access to AMD sideband protocols
 * The APML RMI module checks whether the cmd is
 * - Mailbox message read/write(0x0~0x999)
 * - CPUID read(0x1000)
 * - MCAMSR read(0x1001)
 * - returning "-EFAULT" if none of the above
 * "-EPROTOTYPE" error is returned to provide additional error details
 */
#define SBRMI_IOCTL_CMD		_IOWR(SB_BASE_IOCTL_NR, 0, struct apml_message)

#endif /*_AMD_APML_H_*/
