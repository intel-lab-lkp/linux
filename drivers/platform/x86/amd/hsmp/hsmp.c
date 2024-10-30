// SPDX-License-Identifier: GPL-2.0
/*
 * AMD HSMP Platform Driver
 * Copyright (c) 2022, AMD.
 * All Rights Reserved.
 *
 * This file provides a device implementation for HSMP interface
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <asm/amd_hsmp.h>
#include <asm/amd_nb.h>

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/semaphore.h>
#include <linux/sysfs.h>

#include "hsmp.h"

/* HSMP Status / Error codes */
#define HSMP_STATUS_NOT_READY	0x00
#define HSMP_STATUS_OK		0x01
#define HSMP_ERR_INVALID_MSG	0xFE
#define HSMP_ERR_INVALID_INPUT	0xFF

/* Timeout in millsec */
#define HSMP_MSG_TIMEOUT	100
#define HSMP_SHORT_SLEEP	1

#define HSMP_WR			true
#define HSMP_RD			false

#define DRIVER_VERSION		"2.3"

static struct hsmp_plat_device hsmp_pdev;

/*
 * User may use these comments as reference, please find the
 * supported list of messages and message definition in the
 * HSMP chapter of respective family/model PPR.
 *
 * Not supported messages would return -ENOMSG.
 */
static const struct hsmp_msg_desc hsmp_msg_desc_table[] = {
	/* RESERVED */
	{0, 0, HSMP_RSVD},

	/*
	 * HSMP_TEST, num_args = 1, response_sz = 1
	 * input:  args[0] = xx
	 * output: args[0] = xx + 1
	 */
	{1, 1, HSMP_GET},

	/*
	 * HSMP_GET_SMU_VER, num_args = 0, response_sz = 1
	 * output: args[0] = smu fw ver
	 */
	{0, 1, HSMP_GET},

	/*
	 * HSMP_GET_PROTO_VER, num_args = 0, response_sz = 1
	 * output: args[0] = proto version
	 */
	{0, 1, HSMP_GET},

	/*
	 * HSMP_GET_SOCKET_POWER, num_args = 0, response_sz = 1
	 * output: args[0] = socket power in mWatts
	 */
	{0, 1, HSMP_GET},

	/*
	 * HSMP_SET_SOCKET_POWER_LIMIT, num_args = 1, response_sz = 0
	 * input: args[0] = power limit value in mWatts
	 */
	{1, 0, HSMP_SET},

	/*
	 * HSMP_GET_SOCKET_POWER_LIMIT, num_args = 0, response_sz = 1
	 * output: args[0] = socket power limit value in mWatts
	 */
	{0, 1, HSMP_GET},

	/*
	 * HSMP_GET_SOCKET_POWER_LIMIT_MAX, num_args = 0, response_sz = 1
	 * output: args[0] = maximuam socket power limit in mWatts
	 */
	{0, 1, HSMP_GET},

	/*
	 * HSMP_SET_BOOST_LIMIT, num_args = 1, response_sz = 0
	 * input: args[0] = apic id[31:16] + boost limit value in MHz[15:0]
	 */
	{1, 0, HSMP_SET},

	/*
	 * HSMP_SET_BOOST_LIMIT_SOCKET, num_args = 1, response_sz = 0
	 * input: args[0] = boost limit value in MHz
	 */
	{1, 0, HSMP_SET},

	/*
	 * HSMP_GET_BOOST_LIMIT, num_args = 1, response_sz = 1
	 * input: args[0] = apic id
	 * output: args[0] = boost limit value in MHz
	 */
	{1, 1, HSMP_GET},

	/*
	 * HSMP_GET_PROC_HOT, num_args = 0, response_sz = 1
	 * output: args[0] = proc hot status
	 */
	{0, 1, HSMP_GET},

	/*
	 * HSMP_SET_XGMI_LINK_WIDTH, num_args = 1, response_sz = 0
	 * input: args[0] = min link width[15:8] + max link width[7:0]
	 */
	{1, 0, HSMP_SET},

	/*
	 * HSMP_SET_DF_PSTATE, num_args = 1, response_sz = 0
	 * input: args[0] = df pstate[7:0]
	 */
	{1, 0, HSMP_SET},

	/* HSMP_SET_AUTO_DF_PSTATE, num_args = 0, response_sz = 0 */
	{0, 0, HSMP_SET},

	/*
	 * HSMP_GET_FCLK_MCLK, num_args = 0, response_sz = 2
	 * output: args[0] = fclk in MHz, args[1] = mclk in MHz
	 */
	{0, 2, HSMP_GET},

	/*
	 * HSMP_GET_CCLK_THROTTLE_LIMIT, num_args = 0, response_sz = 1
	 * output: args[0] = core clock in MHz
	 */
	{0, 1, HSMP_GET},

	/*
	 * HSMP_GET_C0_PERCENT, num_args = 0, response_sz = 1
	 * output: args[0] = average c0 residency
	 */
	{0, 1, HSMP_GET},

	/*
	 * HSMP_SET_NBIO_DPM_LEVEL, num_args = 1, response_sz = 0
	 * input: args[0] = nbioid[23:16] + max dpm level[15:8] + min dpm level[7:0]
	 */
	{1, 0, HSMP_SET},

	/*
	 * HSMP_GET_NBIO_DPM_LEVEL, num_args = 1, response_sz = 1
	 * input: args[0] = nbioid[23:16]
	 * output: args[0] = max dpm level[15:8] + min dpm level[7:0]
	 */
	{1, 1, HSMP_GET},

	/*
	 * HSMP_GET_DDR_BANDWIDTH, num_args = 0, response_sz = 1
	 * output: args[0] = max bw in Gbps[31:20] + utilised bw in Gbps[19:8] +
	 * bw in percentage[7:0]
	 */
	{0, 1, HSMP_GET},

	/*
	 * HSMP_GET_TEMP_MONITOR, num_args = 0, response_sz = 1
	 * output: args[0] = temperature in degree celsius. [15:8] integer part +
	 * [7:5] fractional part
	 */
	{0, 1, HSMP_GET},

	/*
	 * HSMP_GET_DIMM_TEMP_RANGE, num_args = 1, response_sz = 1
	 * input: args[0] = DIMM address[7:0]
	 * output: args[0] = refresh rate[3] + temperature range[2:0]
	 */
	{1, 1, HSMP_GET},

	/*
	 * HSMP_GET_DIMM_POWER, num_args = 1, response_sz = 1
	 * input: args[0] = DIMM address[7:0]
	 * output: args[0] = DIMM power in mW[31:17] + update rate in ms[16:8] +
	 * DIMM address[7:0]
	 */
	{1, 1, HSMP_GET},

	/*
	 * HSMP_GET_DIMM_THERMAL, num_args = 1, response_sz = 1
	 * input: args[0] = DIMM address[7:0]
	 * output: args[0] = temperature in degree celsius[31:21] + update rate in ms[16:8] +
	 * DIMM address[7:0]
	 */
	{1, 1, HSMP_GET},

	/*
	 * HSMP_GET_SOCKET_FREQ_LIMIT, num_args = 0, response_sz = 1
	 * output: args[0] = frequency in MHz[31:16] + frequency source[15:0]
	 */
	{0, 1, HSMP_GET},

	/*
	 * HSMP_GET_CCLK_CORE_LIMIT, num_args = 1, response_sz = 1
	 * input: args[0] = apic id [31:0]
	 * output: args[0] = frequency in MHz[31:0]
	 */
	{1, 1, HSMP_GET},

	/*
	 * HSMP_GET_RAILS_SVI, num_args = 0, response_sz = 1
	 * output: args[0] = power in mW[31:0]
	 */
	{0, 1, HSMP_GET},

	/*
	 * HSMP_GET_SOCKET_FMAX_FMIN, num_args = 0, response_sz = 1
	 * output: args[0] = fmax in MHz[31:16] + fmin in MHz[15:0]
	 */
	{0, 1, HSMP_GET},

	/*
	 * HSMP_GET_IOLINK_BANDWITH, num_args = 1, response_sz = 1
	 * input: args[0] = link id[15:8] + bw type[2:0]
	 * output: args[0] = io bandwidth in Mbps[31:0]
	 */
	{1, 1, HSMP_GET},

	/*
	 * HSMP_GET_XGMI_BANDWITH, num_args = 1, response_sz = 1
	 * input: args[0] = link id[15:8] + bw type[2:0]
	 * output: args[0] = xgmi bandwidth in Mbps[31:0]
	 */
	{1, 1, HSMP_GET},

	/*
	 * HSMP_SET_GMI3_WIDTH, num_args = 1, response_sz = 0
	 * input: args[0] = min link width[15:8] + max link width[7:0]
	 */
	{1, 0, HSMP_SET},

	/*
	 * HSMP_SET_PCI_RATE, num_args = 1, response_sz = 1
	 * input: args[0] = link rate control value
	 * output: args[0] = previous link rate control value
	 */
	{1, 1, HSMP_SET},

	/*
	 * HSMP_SET_POWER_MODE, num_args = 1, response_sz = 0
	 * input: args[0] = power efficiency mode[2:0]
	 */
	{1, 0, HSMP_SET},

	/*
	 * HSMP_SET_PSTATE_MAX_MIN, num_args = 1, response_sz = 0
	 * input: args[0] = min df pstate[15:8] + max df pstate[7:0]
	 */
	{1, 0, HSMP_SET},

	/*
	 * HSMP_GET_METRIC_TABLE_VER, num_args = 0, response_sz = 1
	 * output: args[0] = metrics table version
	 */
	{0, 1, HSMP_GET},

	/*
	 * HSMP_GET_METRIC_TABLE, num_args = 0, response_sz = 0
	 */
	{0, 0, HSMP_GET},

	/*
	 * HSMP_GET_METRIC_TABLE_DRAM_ADDR, num_args = 0, response_sz = 2
	 * output: args[0] = lower 32 bits of the address
	 * output: args[1] = upper 32 bits of the address
	 */
	{0, 2, HSMP_GET},
};

/*
 * Send a message to the HSMP port via PCI-e config space registers
 * or by writing to MMIO space.
 *
 * The caller is expected to zero out any unused arguments.
 * If a response is expected, the number of response words should be greater than 0.
 *
 * Returns 0 for success and populates the requested number of arguments.
 * Returns a negative error code for failure.
 */
static int __hsmp_send_message(struct hsmp_socket *sock, struct hsmp_message *msg)
{
	struct hsmp_mbaddr_info *mbinfo;
	unsigned long timeout, short_sleep;
	u32 mbox_status;
	u32 index;
	int ret;

	mbinfo = &sock->mbinfo;

	/* Clear the status register */
	mbox_status = HSMP_STATUS_NOT_READY;
	ret = sock->amd_hsmp_rdwr(sock, mbinfo->msg_resp_off, &mbox_status, HSMP_WR);
	if (ret) {
		pr_err("Error %d clearing mailbox status register\n", ret);
		return ret;
	}

	index = 0;
	/* Write any message arguments */
	while (index < msg->num_args) {
		ret = sock->amd_hsmp_rdwr(sock, mbinfo->msg_arg_off + (index << 2),
					  &msg->args[index], HSMP_WR);
		if (ret) {
			pr_err("Error %d writing message argument %d\n", ret, index);
			return ret;
		}
		index++;
	}

	/* Write the message ID which starts the operation */
	ret = sock->amd_hsmp_rdwr(sock, mbinfo->msg_id_off, &msg->msg_id, HSMP_WR);
	if (ret) {
		pr_err("Error %d writing message ID %u\n", ret, msg->msg_id);
		return ret;
	}

	/*
	 * Depending on when the trigger write completes relative to the SMU
	 * firmware 1 ms cycle, the operation may take from tens of us to 1 ms
	 * to complete. Some operations may take more. Therefore we will try
	 * a few short duration sleeps and switch to long sleeps if we don't
	 * succeed quickly.
	 */
	short_sleep = jiffies + msecs_to_jiffies(HSMP_SHORT_SLEEP);
	timeout	= jiffies + msecs_to_jiffies(HSMP_MSG_TIMEOUT);

	while (time_before(jiffies, timeout)) {
		ret = sock->amd_hsmp_rdwr(sock, mbinfo->msg_resp_off, &mbox_status, HSMP_RD);
		if (ret) {
			pr_err("Error %d reading mailbox status\n", ret);
			return ret;
		}

		if (mbox_status != HSMP_STATUS_NOT_READY)
			break;
		if (time_before(jiffies, short_sleep))
			usleep_range(50, 100);
		else
			usleep_range(1000, 2000);
	}

	if (unlikely(mbox_status == HSMP_STATUS_NOT_READY)) {
		return -ETIMEDOUT;
	} else if (unlikely(mbox_status == HSMP_ERR_INVALID_MSG)) {
		return -ENOMSG;
	} else if (unlikely(mbox_status == HSMP_ERR_INVALID_INPUT)) {
		return -EINVAL;
	} else if (unlikely(mbox_status != HSMP_STATUS_OK)) {
		pr_err("Message ID %u unknown failure (status = 0x%X)\n",
		       msg->msg_id, mbox_status);
		return -EIO;
	}

	/*
	 * SMU has responded OK. Read response data.
	 * SMU reads the input arguments from eight 32 bit registers starting
	 * from SMN_HSMP_MSG_DATA and writes the response data to the same
	 * SMN_HSMP_MSG_DATA address.
	 * We copy the response data if any, back to the args[].
	 */
	index = 0;
	while (index < msg->response_sz) {
		ret = sock->amd_hsmp_rdwr(sock, mbinfo->msg_arg_off + (index << 2),
					  &msg->args[index], HSMP_RD);
		if (ret) {
			pr_err("Error %d reading response %u for message ID:%u\n",
			       ret, index, msg->msg_id);
			break;
		}
		index++;
	}

	return ret;
}

static int validate_message(struct hsmp_message *msg)
{
	/* msg_id against valid range of message IDs */
	if (msg->msg_id < HSMP_TEST || msg->msg_id >= HSMP_MSG_ID_MAX)
		return -ENOMSG;

	/* msg_id is a reserved message ID */
	if (hsmp_msg_desc_table[msg->msg_id].type == HSMP_RSVD)
		return -ENOMSG;

	/* num_args and response_sz against the HSMP spec */
	if (msg->num_args != hsmp_msg_desc_table[msg->msg_id].num_args ||
	    msg->response_sz != hsmp_msg_desc_table[msg->msg_id].response_sz)
		return -EINVAL;

	return 0;
}

int hsmp_send_message(struct hsmp_message *msg)
{
	struct hsmp_socket *sock;
	int ret;

	if (!msg)
		return -EINVAL;
	ret = validate_message(msg);
	if (ret)
		return ret;

	if (!hsmp_pdev.sock || msg->sock_ind >= hsmp_pdev.num_sockets)
		return -ENODEV;
	sock = &hsmp_pdev.sock[msg->sock_ind];

	/*
	 * The time taken by smu operation to complete is between
	 * 10us to 1ms. Sometime it may take more time.
	 * In SMP system timeout of 100 millisecs should
	 * be enough for the previous thread to finish the operation
	 */
	ret = down_timeout(&sock->hsmp_sem, msecs_to_jiffies(HSMP_MSG_TIMEOUT));
	if (ret < 0)
		return ret;

	ret = __hsmp_send_message(sock, msg);

	up(&sock->hsmp_sem);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(hsmp_send_message, AMD_HSMP);

int hsmp_test(u16 sock_ind, u32 value)
{
	struct hsmp_message msg = { 0 };
	int ret;

	/*
	 * Test the hsmp port by performing TEST command. The test message
	 * takes one argument and returns the value of that argument + 1.
	 */
	msg.msg_id	= HSMP_TEST;
	msg.num_args	= 1;
	msg.response_sz	= 1;
	msg.args[0]	= value;
	msg.sock_ind	= sock_ind;

	ret = hsmp_send_message(&msg);
	if (ret)
		return ret;

	/* Check the response value */
	if (msg.args[0] != (value + 1)) {
		dev_err(hsmp_pdev.sock[sock_ind].dev,
			"Socket %d test message failed, Expected 0x%08X, received 0x%08X\n",
			sock_ind, (value + 1), msg.args[0]);
		return -EBADE;
	}

	return ret;
}
EXPORT_SYMBOL_NS_GPL(hsmp_test, AMD_HSMP);

long hsmp_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	int __user *arguser = (int  __user *)arg;
	struct hsmp_message msg = { 0 };
	int ret;

	if (copy_struct_from_user(&msg, sizeof(msg), arguser, sizeof(struct hsmp_message)))
		return -EFAULT;

	/*
	 * Check msg_id is within the range of supported msg ids
	 * i.e within the array bounds of hsmp_msg_desc_table
	 */
	if (msg.msg_id < HSMP_TEST || msg.msg_id >= HSMP_MSG_ID_MAX)
		return -ENOMSG;

	switch (fp->f_mode & (FMODE_WRITE | FMODE_READ)) {
	case FMODE_WRITE:
		/*
		 * Device is opened in O_WRONLY mode
		 * Execute only set/configure commands
		 */
		if (hsmp_msg_desc_table[msg.msg_id].type != HSMP_SET)
			return -EINVAL;
		break;
	case FMODE_READ:
		/*
		 * Device is opened in O_RDONLY mode
		 * Execute only get/monitor commands
		 */
		if (hsmp_msg_desc_table[msg.msg_id].type != HSMP_GET)
			return -EINVAL;
		break;
	case FMODE_READ | FMODE_WRITE:
		/*
		 * Device is opened in O_RDWR mode
		 * Execute both get/monitor and set/configure commands
		 */
		break;
	default:
		return -EINVAL;
	}

	ret = hsmp_send_message(&msg);
	if (ret)
		return ret;

	if (hsmp_msg_desc_table[msg.msg_id].response_sz > 0) {
		/* Copy results back to user for get/monitor commands */
		if (copy_to_user(arguser, &msg, sizeof(struct hsmp_message)))
			return -EFAULT;
	}

	return 0;
}

ssize_t hsmp_metric_tbl_read(struct hsmp_socket *sock, char *buf, size_t size)
{
	struct hsmp_message msg = { 0 };
	int ret;

	if (!sock || !buf)
		return -EINVAL;

	/* Do not support lseek(), also don't allow more than the size of metric table */
	if (size != sizeof(struct hsmp_metric_table)) {
		dev_err(sock->dev, "Wrong buffer size\n");
		return -EINVAL;
	}

	msg.msg_id	= HSMP_GET_METRIC_TABLE;
	msg.sock_ind	= sock->sock_ind;

	ret = hsmp_send_message(&msg);
	if (ret)
		return ret;
	memcpy_fromio(buf, sock->metric_tbl_addr, size);

	return size;
}
EXPORT_SYMBOL_NS_GPL(hsmp_metric_tbl_read, AMD_HSMP);

int hsmp_get_tbl_dram_base(u16 sock_ind)
{
	struct hsmp_socket *sock = &hsmp_pdev.sock[sock_ind];
	struct hsmp_message msg = { 0 };
	phys_addr_t dram_addr;
	int ret;

	msg.sock_ind	= sock_ind;
	msg.response_sz	= hsmp_msg_desc_table[HSMP_GET_METRIC_TABLE_DRAM_ADDR].response_sz;
	msg.msg_id	= HSMP_GET_METRIC_TABLE_DRAM_ADDR;

	ret = hsmp_send_message(&msg);
	if (ret)
		return ret;

	/*
	 * calculate the metric table DRAM address from lower and upper 32 bits
	 * sent from SMU and ioremap it to virtual address.
	 */
	dram_addr = msg.args[0] | ((u64)(msg.args[1]) << 32);
	if (!dram_addr) {
		dev_err(sock->dev, "Invalid DRAM address for metric table\n");
		return -ENOMEM;
	}
	sock->metric_tbl_addr = devm_ioremap(sock->dev, dram_addr,
					     sizeof(struct hsmp_metric_table));
	if (!sock->metric_tbl_addr) {
		dev_err(sock->dev, "Failed to ioremap metric table addr\n");
		return -ENOMEM;
	}
	return 0;
}
EXPORT_SYMBOL_NS_GPL(hsmp_get_tbl_dram_base, AMD_HSMP);

int hsmp_cache_proto_ver(u16 sock_ind)
{
	struct hsmp_message msg = { 0 };
	int ret;

	msg.msg_id	= HSMP_GET_PROTO_VER;
	msg.sock_ind	= sock_ind;
	msg.response_sz = hsmp_msg_desc_table[HSMP_GET_PROTO_VER].response_sz;

	ret = hsmp_send_message(&msg);
	if (!ret)
		hsmp_pdev.proto_ver = msg.args[0];

	return ret;
}
EXPORT_SYMBOL_NS_GPL(hsmp_cache_proto_ver, AMD_HSMP);

static const struct file_operations hsmp_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= hsmp_ioctl,
	.compat_ioctl	= hsmp_ioctl,
};

int hsmp_misc_register(struct device *dev)
{
	hsmp_pdev.mdev.name	= HSMP_CDEV_NAME;
	hsmp_pdev.mdev.minor	= MISC_DYNAMIC_MINOR;
	hsmp_pdev.mdev.fops	= &hsmp_fops;
	hsmp_pdev.mdev.parent	= dev;
	hsmp_pdev.mdev.nodename	= HSMP_DEVNODE_NAME;
	hsmp_pdev.mdev.mode	= 0644;

	return misc_register(&hsmp_pdev.mdev);
}
EXPORT_SYMBOL_NS_GPL(hsmp_misc_register, AMD_HSMP);

void hsmp_misc_deregister(void)
{
	misc_deregister(&hsmp_pdev.mdev);
}
EXPORT_SYMBOL_NS_GPL(hsmp_misc_deregister, AMD_HSMP);

struct hsmp_plat_device *get_hsmp_pdev(void)
{
	return &hsmp_pdev;
}
EXPORT_SYMBOL_NS_GPL(get_hsmp_pdev, AMD_HSMP);

MODULE_DESCRIPTION("AMD HSMP Common driver");
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");
