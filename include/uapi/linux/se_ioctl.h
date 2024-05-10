/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause*/
/*
 * Copyright 2024 NXP
 */

#ifndef SE_IOCTL_H
#define SE_IOCTL_H

/* IOCTL definitions. */

struct se_ioctl_setup_iobuf {
	u8 *user_buf;
	u32 length;
	u32 flags;
	u64 ele_addr;
};

struct se_ioctl_shared_mem_cfg {
	u32 base_offset;
	u32 size;
};

struct se_ioctl_get_if_info {
	u8 se_if_id;
	u8 interrupt_idx;
	u8 tz;
	u8 did;
	u8 cmd_tag;
	u8 rsp_tag;
	u8 success_tag;
	u8 base_api_ver;
	u8 fw_api_ver;
};

struct se_ioctl_signed_message {
	u8 *message;
	u32 msg_size;
	u32 error_code;
};

struct se_ioctl_get_soc_info {
	u16 soc_id;
	u16 soc_rev;
};

/* IO Buffer Flags */
#define SE_IO_BUF_FLAGS_IS_OUTPUT	(0x00u)
#define SE_IO_BUF_FLAGS_IS_INPUT	(0x01u)
#define SE_IO_BUF_FLAGS_USE_SEC_MEM	(0x02u)
#define SE_IO_BUF_FLAGS_USE_SHORT_ADDR	(0x04u)
#define SE_IO_BUF_FLAGS_IS_IN_OUT	(0x10u)

/* IOCTLS */
#define SE_IOCTL			0x0A /* like MISC_MAJOR. */

/*
 * ioctl to designated the current fd as logical-reciever.
 * This is ioctl is send when the nvm-daemon, a slave to the
 * firmware is started by the user.
 */
#define SE_IOCTL_ENABLE_CMD_RCV	_IO(SE_IOCTL, 0x01)

/*
 * ioctl to get the buffer allocated from the memory, which is shared
 * between kernel and FW.
 * Post allocation, the kernel tagged the allocated memory with:
 *  Output
 *  Input
 *  Input-Output
 *  Short address
 *  Secure-memory
 */
#define SE_IOCTL_SETUP_IOBUF	_IOWR(SE_IOCTL, 0x03, \
					struct se_ioctl_setup_iobuf)

/*
 * ioctl to get the mu information, that is used to exchange message
 * with FW, from user-spaced.
 */
#define SE_IOCTL_GET_MU_INFO	_IOR(SE_IOCTL, 0x04, \
					struct se_ioctl_get_if_info)
/*
 * ioctl to get SoC Info from user-space.
 */
#define SE_IOCTL_GET_SOC_INFO      _IOR(SE_IOCTL, 0x06, \
					struct se_ioctl_get_soc_info)

#endif
