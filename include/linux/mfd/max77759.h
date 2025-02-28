/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2020 Google Inc.
 * Copyright 2025 Linaro Ltd.
 *
 * Client interface for Maxim MAX77759 MFD driver
 */

#ifndef __LINUX_MFD_MAX77759_H
#define __LINUX_MFD_MAX77759_H

/* MaxQ opcodes */
#define MAX77759_MAXQ_OPCODE_MAXLENGTH 33

#define MAX77759_MAXQ_OPCODE_GPIO_TRIGGER_READ   0x21
#define MAX77759_MAXQ_OPCODE_GPIO_TRIGGER_WRITE  0x22
#define MAX77759_MAXQ_OPCODE_GPIO_CONTROL_READ   0x23
#define MAX77759_MAXQ_OPCODE_GPIO_CONTROL_WRITE  0x24
#define MAX77759_MAXQ_OPCODE_USER_SPACE_READ     0x81
#define MAX77759_MAXQ_OPCODE_USER_SPACE_WRITE    0x82

/*
 * register map (incomplete) - registers not useful for drivers are not
 * declared here
 */
/* MaxQ */
#define MAX77759_MAXQ_REG_UIC_INT1            0x64
#define MAX77759_MAXQ_REG_UIC_INT1_APCMDRESI  BIT(7)
#define MAX77759_MAXQ_REG_UIC_INT1_SYSMSGI    BIT(6)
#define MAX77759_MAXQ_REG_UIC_INT1_GPIO6I     BIT(1)
#define MAX77759_MAXQ_REG_UIC_INT1_GPIO5I     BIT(0)
#define MAX77759_MAXQ_REG_UIC_INT1_GPIOxI(offs, en)  (((en) & 1) << (offs))
#define MAX77759_MAXQ_REG_UIC_INT1_GPIOxI_MASK(offs) \
				MAX77759_MAXQ_REG_UIC_INT1_GPIOxI(offs, ~0)

#define MAX77759_MAXQ_REG_UIC_INT2            0x65
#define MAX77759_MAXQ_REG_UIC_INT3            0x66
#define MAX77759_MAXQ_REG_UIC_INT4            0x67
#define MAX77759_MAXQ_REG_UIC_UIC_STATUS1     0x68
#define MAX77759_MAXQ_REG_UIC_UIC_STATUS2     0x69
#define MAX77759_MAXQ_REG_UIC_UIC_STATUS3     0x6a
#define MAX77759_MAXQ_REG_UIC_UIC_STATUS4     0x6b
#define MAX77759_MAXQ_REG_UIC_UIC_STATUS5     0x6c
#define MAX77759_MAXQ_REG_UIC_UIC_STATUS6     0x6d
#define MAX77759_MAXQ_REG_UIC_UIC_STATUS7     0x6f
#define MAX77759_MAXQ_REG_UIC_UIC_STATUS8     0x6f
#define MAX77759_MAXQ_REG_UIC_INT1_M          0x70
#define MAX77759_MAXQ_REG_UIC_INT2_M          0x71
#define MAX77759_MAXQ_REG_UIC_INT3_M          0x72
#define MAX77759_MAXQ_REG_UIC_INT4_M          0x73

/* charger */
#define MAX77759_CHGR_REG_CHG_INT        0xb0
#define MAX77759_CHGR_REG_CHG_INT2       0xb1
#define MAX77759_CHGR_REG_CHG_INT_MASK   0xb2
#define MAX77759_CHGR_REG_CHG_INT2_MASK  0xb3

struct max77759_mfd;

/**
 * struct max77759_maxq_command - structure containing the MaxQ command to
 * send
 *
 * @length: The number of bytes to send.
 * @cmd: The data to send.
 */
struct max77759_maxq_command {
	u8 length;
	u8 cmd[] __counted_by(length);
};

/**
 * struct max77759_maxq_response - structure containing the MaxQ response
 *
 * @length: The number of bytes to receive.
 * @rsp: The data received. Must have at least @length bytes space.
 */
struct max77759_maxq_response {
	u8 length;
	u8 rsp[] __counted_by(length);
};

/**
 * max77759_maxq_command() - issue a MaxQ command and wait for the response
 * and associated data
 *
 * @max77759_mfd: The core max77759 mfd device handle.
 * @cmd: The command to be sent.
 * @rsp: Any response data associated with the command will be copied here;
 *     can be %NULL if the command has no response (other than ACK).
 *
 * Return: 0 on success, a negative error number otherwise.
 */
int max77759_maxq_command(struct max77759_mfd *max77759_mfd,
			  const struct max77759_maxq_command *cmd,
			  struct max77759_maxq_response *rsp);

#endif /* __LINUX_MFD_MAX77759_H */
