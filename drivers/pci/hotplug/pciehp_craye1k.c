// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2022-2024 Lawrence Livermore National Security, LLC
 */
/*
 * Cray ClusterStor E1000 hotplug slot LED driver extensions
 *
 * This driver controls the NVMe slot LEDs on the Cray ClusterStore E1000.
 * It provides hotplug attention status callbacks for the 24 NVMe slots on
 * the E1000.  This allows users to access the E1000's locate and fault
 * LEDs via the normal /sys/bus/pci/slots/<slot>/attention sysfs entries.
 * This driver uses IPMI to communicate with the E1000 controller to toggle
 * the LEDs.
 *
 * This driver is based off of ibmpex.c
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/dmi.h>
#include <linux/ipmi.h>
#include <linux/ipmi_smi.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>
#include <linux/random.h>
#include "pciehp.h"

/* Cray E1000 commands */
#define CRAYE1K_CMD_NETFN       0x3c
#define CRAYE1K_CMD_PRIMARY     0x33
#define CRAYE1K_CMD_FAULT_LED   0x39
#define CRAYE1K_CMD_LOCATE_LED  0x22

/* Subcommands */
#define CRAYE1K_GET_LED		0x0
#define CRAYE1K_SET_LED		0x1
#define CRAYE1K_SET_PRIMARY		0x1

/*
 * Milliseconds to wait after get/set LED command.  200ms value found though
 * experimentation
 */
#define	CRAYE1K_POST_CMD_WAIT_MS	200

struct craye1k {
	struct device *dev;   /* BMC device */
	struct mutex lock;
	struct completion read_complete;
	struct ipmi_addr address;
	struct ipmi_user *user;
	int iface;

	long tx_msg_id;
	struct kernel_ipmi_msg tx_msg;
	unsigned char tx_msg_data[IPMI_MAX_MSG_LENGTH];
	unsigned char rx_msg_data[IPMI_MAX_MSG_LENGTH];
	unsigned long rx_msg_len;
	unsigned char rx_result;	/* IPMI completion code */

	/* Parent dir for all our debugfs entries */
	struct dentry *parent;

	/* debugfs stats */
	u64 check_primary;
	u64 check_primary_failed;
	u64 was_already_primary;
	u64 was_not_already_primary;
	u64 set_primary;
	u64 set_initial_primary_failed;
	u64 set_primary_failed;
	u64 set_led_locate_failed;
	u64 set_led_fault_failed;
	u64 set_led_readback_failed;
	u64 set_led_failed;
	u64 get_led_failed;
	u64 completion_timeout;
	u64 wrong_msgid;
	u64 request_failed;

	/* debugfs configuration options */

	/* Print info on spurious IPMI messages */
	bool print_errors;

	/* Retries for kernel IPMI layer */
	u32 ipmi_retries;

	/* Timeout in ms for IPMI (0 = use IPMI default_retry_ms) */
	u32 ipmi_timeout_ms;

	/* Timeout in ms to wait for E1000 message completion */
	u32 completion_timeout_ms;
};

/*
 * Make our craye1k a global so get/set_attention_status() can access it.
 * This is safe since there's only one node controller on the board, and so it's
 * impossible to instantiate more than one craye1k.
 */
static struct craye1k *craye1k_global;

/* Return parent dir dentry */
static struct dentry *
craye1k_debugfs_init(struct craye1k *craye1k)
{
	umode_t mode = 0644;
	struct dentry *parent = debugfs_create_dir("pciehp_craye1k", NULL);

	if (!parent)
		return NULL;

	debugfs_create_x64("check_primary", mode, parent,
			   &craye1k->check_primary);
	debugfs_create_x64("check_primary_failed", mode, parent,
			   &craye1k->check_primary_failed);
	debugfs_create_x64("was_already_primary", mode, parent,
			   &craye1k->was_already_primary);
	debugfs_create_x64("was_not_already_primary", mode, parent,
			   &craye1k->was_not_already_primary);
	debugfs_create_x64("set_primary", mode, parent,
			   &craye1k->set_primary);
	debugfs_create_x64("set_initial_primary_failed", mode, parent,
			   &craye1k->set_initial_primary_failed);
	debugfs_create_x64("set_primary_failed", mode, parent,
			   &craye1k->set_primary_failed);
	debugfs_create_x64("set_led_locate_failed", mode, parent,
			   &craye1k->set_led_locate_failed);
	debugfs_create_x64("set_led_fault_failed", mode, parent,
			   &craye1k->set_led_fault_failed);
	debugfs_create_x64("set_led_readback_failed", mode, parent,
			   &craye1k->set_led_readback_failed);
	debugfs_create_x64("set_led_failed", mode, parent,
			   &craye1k->set_led_failed);
	debugfs_create_x64("get_led_failed", mode, parent,
			   &craye1k->get_led_failed);
	debugfs_create_x64("completion_timeout", mode, parent,
			   &craye1k->completion_timeout);
	debugfs_create_x64("wrong_msgid", mode, parent,
			   &craye1k->wrong_msgid);
	debugfs_create_x64("request_failed", mode, parent,
			   &craye1k->request_failed);

	debugfs_create_x32("ipmi_retries", mode, parent,
			   &craye1k->ipmi_retries);
	debugfs_create_x32("ipmi_timeout_ms", mode, parent,
			   &craye1k->ipmi_timeout_ms);
	debugfs_create_x32("completion_timeout_ms", mode, parent,
			   &craye1k->completion_timeout_ms);
	debugfs_create_bool("print_errors", mode, parent,
			    &craye1k->print_errors);

	return parent;
}

/*
 * craye1k_msg_handler() - IPMI message response handler
 */
static void craye1k_msg_handler(struct ipmi_recv_msg *msg, void *user_msg_data)
{
	struct craye1k *craye1k = user_msg_data;

	if (msg->msgid != craye1k->tx_msg_id) {
		craye1k->wrong_msgid++;
		if (craye1k->print_errors) {
			dev_warn_ratelimited(craye1k->dev, "rx msgid %d != %d",
					     (int)msg->msgid,
					     (int)craye1k->tx_msg_id);
		}
		ipmi_free_recv_msg(msg);
		return;
	}

	/* Set rx_result to the IPMI completion code */
	if (msg->msg.data_len > 0)
		craye1k->rx_result = msg->msg.data[0];
	else
		craye1k->rx_result = IPMI_UNKNOWN_ERR_COMPLETION_CODE;

	if (msg->msg.data_len > 1) {
		/* Exclude completion code from data bytes */
		craye1k->rx_msg_len = msg->msg.data_len - 1;
		memcpy(craye1k->rx_msg_data, msg->msg.data + 1,
		       craye1k->rx_msg_len);
	} else {
		craye1k->rx_msg_len = 0;
	}

	ipmi_free_recv_msg(msg);

	complete(&craye1k->read_complete);
}

static const struct ipmi_user_hndl craye1k_user_hndl = {
	.ipmi_recv_hndl = craye1k_msg_handler
};

static void craye1k_new_smi(int iface, struct device *dev)
{
	int rc;
	struct craye1k *craye1k;

	/* There's only one node controller so driver data should not be set */
	WARN_ON(craye1k_global);

	craye1k = kzalloc(sizeof(*craye1k), GFP_KERNEL);
	if (!craye1k)
		return;

	craye1k->address.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
	craye1k->address.channel = IPMI_BMC_CHANNEL;
	craye1k->iface = iface;
	craye1k->dev = dev;
	craye1k->tx_msg.data = craye1k->tx_msg_data;
	craye1k->ipmi_retries = 4;
	craye1k->ipmi_timeout_ms = 500;
	craye1k->completion_timeout_ms = 300;

	init_completion(&craye1k->read_complete);
	mutex_init(&craye1k->lock);

	dev_set_drvdata(dev, craye1k);

	rc = ipmi_create_user(craye1k->iface, &craye1k_user_hndl, craye1k,
			      &craye1k->user);
	if (rc < 0) {
		dev_err_ratelimited(dev,
				    "Unable to register IPMI user, iface %d\n",
				    craye1k->iface);
		kfree(craye1k);
		dev_set_drvdata(dev, NULL);
		return;
	}

	craye1k_global = craye1k;

	craye1k->parent = craye1k_debugfs_init(craye1k);
	if (!craye1k->parent)
		dev_warn_ratelimited(dev, "Cannot create debugfs");

	dev_info_ratelimited(dev,
			     "Cray ClusterStor E1000 slot LEDs registered");
}

static void craye1k_smi_gone(int iface)
{
	pr_warn("craye1k: Got unexpected smi_gone, iface=%d", iface);
}

static struct ipmi_smi_watcher craye1k_smi_watcher = {
	.owner = THIS_MODULE,
	.new_smi = craye1k_new_smi,
	.smi_gone = craye1k_smi_gone
};

/*
 * craye1k_send_message() - Send the message already setup in 'craye1k'
 *
 * Context: craye1k->lock is already held.
 * Return: 0 on success, non-zero on error.
 */
static int craye1k_send_message(struct craye1k *craye1k)
{
	int rc;

	rc = ipmi_validate_addr(&craye1k->address, sizeof(craye1k->address));
	if (rc) {
		dev_err_ratelimited(craye1k->dev, "validate_addr() = %d\n", rc);
		return rc;
	}

	craye1k->tx_msg_id++;

	rc = ipmi_request_settime(craye1k->user, &craye1k->address,
				  craye1k->tx_msg_id, &craye1k->tx_msg, craye1k,
				  0, craye1k->ipmi_retries,
				  craye1k->ipmi_timeout_ms);

	if (rc) {
		craye1k->request_failed++;
		return rc;
	}

	return 0;
}

/*
 * craye1k_do_message() - Send the message in 'craye1k' and wait for a response
 *
 * Context: craye1k->lock is already held.
 * Return: 0 on success, non-zero on error.
 */
static int craye1k_do_message(struct craye1k *craye1k)
{
	int rc;
	long rc2;
	struct completion *read_complete = &craye1k->read_complete;
	unsigned long tout = msecs_to_jiffies(craye1k->completion_timeout_ms);

	rc = craye1k_send_message(craye1k);
	if (rc == 0) {
		rc2 = wait_for_completion_killable_timeout(read_complete, tout);
		if (rc2 == 0) {
			craye1k->completion_timeout++;
			rc = -ETIME;
		}
	}

	if (craye1k->rx_result == IPMI_UNKNOWN_ERR_COMPLETION_CODE || rc2 <= 0)
		rc = -1;

	return rc;
}

/*
 * __craye1k_do_command() - Do an IPMI command
 *
 * Send a command with optional data bytes, and read back response bytes.
 * Context: craye1k->lock is already held.
 * Returns: 0 on success, non-zero on error.
 */
static int __craye1k_do_command(struct craye1k *craye1k, u8 netfn, u8 cmd,
				u8 *send_data, u8 send_data_len, u8 *recv_data,
				u8 recv_data_len)
{
	int rc;

	craye1k->tx_msg.netfn = netfn;
	craye1k->tx_msg.cmd = cmd;

	if (send_data) {
		memcpy(&craye1k->tx_msg_data[0], send_data, send_data_len);
		craye1k->tx_msg.data_len = send_data_len;
	} else {
		craye1k->tx_msg_data[0] = 0;
		craye1k->tx_msg.data_len = 0;
	}

	rc = craye1k_do_message(craye1k);
	memcpy(recv_data, craye1k->rx_msg_data, recv_data_len);

	return rc;
}

/*
 * craye1k_do_command_and_netfn() - Do IPMI command and return 1st data byte
 *
 * Do an IPMI command with the given netfn, cmd, and optional send payload
 * bytes.
 *
 * Context: craye1k->lock is already held.
 * Returns: the last byte from the response or 0 if response had no response
 * data bytes, else -1 on error.
 */
static int craye1k_do_command_and_netfn(struct craye1k *craye1k, u8 netfn,
					u8 cmd, u8 *send_data, u8 send_data_len)
{
	int rc;

	rc = __craye1k_do_command(craye1k, netfn, cmd, send_data, send_data_len,
				  NULL, 0);
	if (rc != 0) {
		/* Error attempting command */
		return -1;
	}

	if (craye1k->tx_msg.data_len == 0)
		return 0;

	/* Return last received byte value */
	return craye1k->rx_msg_data[craye1k->rx_msg_len - 1];
}

/*
 * craye1k_do_command() - Do a Cray E1000 specific IPMI command.
 * @cmd: Cray E1000 specific command
 * @send_data:  Data to send after the command
 * @send_data_len: Data length
 *
 * Context: craye1k->lock is already held.
 * Returns: the last byte from the response or 0 if response had no response
 * data bytes, else -1 on error.
 */
static int craye1k_do_command(struct craye1k *craye1k, u8 cmd, u8 *send_data,
			      u8 send_data_len)
{
	return (craye1k_do_command_and_netfn(craye1k, CRAYE1K_CMD_NETFN, cmd,
					     send_data, send_data_len));
}

/*
 * __craye1k_set_primary() - Tell the BMC we want to be the primary server
 *
 * An E1000 board has two physical servers on it.  In order to set a slot
 * NVMe LED, this server needs to first tell the BMC that it's the primary
 * server.
 *
 * Returns: 0 on success, non-zero on error.
 */
static int __craye1k_set_primary(struct craye1k *craye1k)
{
	u8 bytes[2] = {CRAYE1K_SET_PRIMARY, 1};	/* set primary to 1 */

	craye1k->set_primary++;
	return craye1k_do_command(craye1k, CRAYE1K_CMD_PRIMARY, bytes, 2);
}

/*
 * craye1k_is_primary() - Are we the primary server?
 *
 * Returns: 1 if we are the primary server, 0 otherwise.
 */
static int craye1k_is_primary(struct craye1k *craye1k)
{
	u8 byte = 0;
	int rc;

	/* Response byte is 0x1 on success */
	rc = craye1k_do_command(craye1k, CRAYE1K_CMD_PRIMARY, &byte, 1);
	craye1k->check_primary++;
	if (rc == 0x1)
		return 1;   /* success */

	craye1k->check_primary_failed++;
	return 0;   /* We are not the primary server node */
}

/*
 * craye1k_set_primary() - Attempt to set ourselves as the primary server
 *
 * Returns: 0 on success, 1 otherwise.
 */
static int craye1k_set_primary(struct craye1k *craye1k)
{
	int tries = 10;

	if (craye1k_is_primary(craye1k)) {
		craye1k->was_already_primary++;
		return 0;
	}
	craye1k->was_not_already_primary++;

	/* delay found through experimentation */
	msleep(300);

	if (__craye1k_set_primary(craye1k) != 0) {
		craye1k->set_initial_primary_failed++;
		return 1;	/* error */
	}

	/*
	 * It can take 2 to 3 seconds after setting primary for the controller
	 * to report that it is the primary.
	 */
	while (tries--) {
		msleep(500);
		if (craye1k_is_primary(craye1k))
			break;
	}

	if (tries == 0) {
		craye1k->set_primary_failed++;
		return 1;	/* never reported that it's primary */
	}

	/* Wait for primary switch to finish */
	msleep(1500);

	return 0;
}

/*
 * craye1k_get_slot_led() - Get slot LED value
 * @slot: Slot number (1-24)
 * @is_locate_led: 0 = get fault LED value, 1 = get locate LED value
 *
 * Returns: slot value on success, -1 on failure.
 */
static int craye1k_get_slot_led(struct craye1k *craye1k, unsigned char slot,
				bool is_locate_led)
{
	u8 bytes[2];
	u8 cmd;

	bytes[0] = CRAYE1K_GET_LED;
	bytes[1] = slot;

	cmd = is_locate_led ? CRAYE1K_CMD_LOCATE_LED : CRAYE1K_CMD_FAULT_LED;

	return craye1k_do_command(craye1k, cmd, bytes, 2);
}

/*
 * craye1k_set_slot_led() - Attempt to set the locate/fault LED to a value
 * @slot: Slot number (1-24)
 * @is_locate_led: 0 = use fault LED, 1 = use locate LED
 * @value: Value to set (0 or 1)
 *
 * Check the LED value after calling this function to ensure it has been set
 * properly.
 *
 * Returns: 0 on success, non-zero on failure.
 */
static int craye1k_set_slot_led(struct craye1k *craye1k, unsigned char slot,
				unsigned char is_locate_led,
				unsigned char value)
{
	u8 bytes[3];
	u8 cmd;

	bytes[0] = CRAYE1K_SET_LED;
	bytes[1] = slot;
	bytes[2] = value;

	cmd = is_locate_led ? CRAYE1K_CMD_LOCATE_LED : CRAYE1K_CMD_FAULT_LED;

	return craye1k_do_command(craye1k, cmd, bytes, 3);
}

static int __craye1k_get_attention_status(struct hotplug_slot *hotplug_slot,
					  u8 *status, bool set_primary)
{
	unsigned char slot;
	int locate, fault;
	struct craye1k *craye1k;

	craye1k = craye1k_global;
	slot = PSN(to_ctrl(hotplug_slot));

	if (set_primary) {
		if (craye1k_set_primary(craye1k) != 0) {
			craye1k->get_led_failed++;
			return -EIO;
		}
	}

	locate = craye1k_get_slot_led(craye1k, slot, true);
	if (locate == -1) {
		craye1k->get_led_failed++;
		return -EINVAL;
	}
	msleep(CRAYE1K_POST_CMD_WAIT_MS);

	fault = craye1k_get_slot_led(craye1k, slot, false);
	if (fault == -1) {
		craye1k->get_led_failed++;
		return -EINVAL;
	}
	msleep(CRAYE1K_POST_CMD_WAIT_MS);

	*status = locate << 1 | fault;

	return 0;
}

int craye1k_get_attention_status(struct hotplug_slot *hotplug_slot,
				 u8 *status)
{
	int rc;
	struct craye1k *craye1k;

	craye1k = craye1k_global;

	if (mutex_lock_interruptible(&craye1k->lock) != 0)
		return -EINTR;

	rc =  __craye1k_get_attention_status(hotplug_slot, status, true);

	mutex_unlock(&craye1k->lock);
	return rc;
}

int craye1k_set_attention_status(struct hotplug_slot *hotplug_slot,
				 u8 status)
{
	unsigned char slot;
	int tries = 4;
	int rc;
	u8 new_status;
	struct craye1k *craye1k;
	bool locate, fault;

	craye1k = craye1k_global;

	slot = PSN(to_ctrl(hotplug_slot));

	if (mutex_lock_interruptible(&craye1k->lock) != 0)
		return -EINTR;

	/* Retry to ensure all LEDs are set */
	while (tries--) {
		/*
		 * The node must first set itself to be the primary node before
		 * setting the slot LEDs (each board has two nodes, or
		 * "servers" as they're called by the manufacturer).  This can
		 * lead to contention if both nodes are trying to set the LEDs
		 * at the same time.
		 */
		rc = craye1k_set_primary(craye1k);
		if (rc != 0) {
			/* Could not set as primary node.  Just retry again. */
			continue;
		}

		/* Write value twice to increase success rate */
		locate = (status & 0x2) >> 1;
		craye1k_set_slot_led(craye1k, slot, 1, locate);
		if (craye1k_set_slot_led(craye1k, slot, 1, locate) != 0) {
			craye1k->set_led_locate_failed++;
			continue;	/* fail, retry */
		}

		msleep(CRAYE1K_POST_CMD_WAIT_MS);

		fault = status & 0x1;
		craye1k_set_slot_led(craye1k, slot, 0, fault);
		if (craye1k_set_slot_led(craye1k, slot, 0, fault) != 0) {
			craye1k->set_led_fault_failed++;
			continue;	/* fail, retry */
		}

		msleep(CRAYE1K_POST_CMD_WAIT_MS);

		rc = __craye1k_get_attention_status(hotplug_slot, &new_status,
						    false);

		msleep(CRAYE1K_POST_CMD_WAIT_MS);

		if (rc == 0 && new_status == status)
			break;	/* success */

		craye1k->set_led_readback_failed++;

		/*
		 * At this point we weren't successful in setting the LED and
		 * need to try again.
		 *
		 * Do a random back-off to reduce contention with other server
		 * node in the unlikely case that both server nodes are trying to
		 * trying to set a LED at the same time.
		 *
		 * The 500ms minimum in the back-off reduced the chance of this
		 * whole retry loop failing from 1 in 700 to none in 10000.
		 */
		msleep(500 + (get_random_long() % 500));
	}
	mutex_unlock(&craye1k->lock);
	if (tries == 0) {
		craye1k->set_led_failed++;
		return -EIO;
	}

	return 0;
}

static bool is_craye1k_board(void)
{
	return dmi_match(DMI_PRODUCT_NAME, "VSSEP1EC");
}

bool is_craye1k_slot(struct controller *ctrl)
{
	return (PSN(ctrl) >= 1 && PSN(ctrl) <= 24 && is_craye1k_board());
}

int craye1k_init(void)
{
	if (!is_craye1k_board())
		return 0;

	return ipmi_smi_watcher_register(&craye1k_smi_watcher);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tony Hutter <hutter2@llnl.gov>");
MODULE_DESCRIPTION("Cray E1000 NVMe Slot LED driver");
