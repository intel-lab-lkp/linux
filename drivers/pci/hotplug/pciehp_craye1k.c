// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2022-2024 Lawrence Livermore National Security, LLC
 */
/*
 * Cray ClusterStor E1000 hotplug slot LED driver
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

#include <linux/delay.h>
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

static bool print_errors;
module_param(print_errors, bool, 0644);
MODULE_PARM_DESC(print_errors, "Print info on spurious IPMI messages");

static int ipmi_retries;
module_param(ipmi_retries, int, 0644);
MODULE_PARM_DESC(ipmi_retries,
		 "Retries for kernel IPMI layer (-1 = use default_max_retries)");

static int ipmi_timeout = 500;
module_param(ipmi_timeout, int, 0644);
MODULE_PARM_DESC(ipmi_timeout,
		 "Timeout in ms for IPMI (0 = use IPMI default_retry_ms)");

static int completion_timeout = 500;
module_param(completion_timeout, int, 0644);
MODULE_PARM_DESC(completion_timeout,
		 "Timeout in ms to wait for E1000 message completion");

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

	/*
	 * Record the original set_attention_status()/get_attention_status()
	 * callbacks for the 24 ports so that we can restore them when we
	 * remove the module.
	 */
	const struct hotplug_slot_ops *orig_ops[24];
	struct hotplug_slot_ops new_ops[24];

	/*
	 * Debugging stats: /sys/class/ipmi/ipmi0/device/craye1k_stats/
	 */
	atomic64_t check_primary;
	atomic64_t check_primary_failed;
	atomic64_t was_already_primary;
	atomic64_t was_not_already_primary;
	atomic64_t set_primary;
	atomic64_t set_initial_primary_failed;
	atomic64_t set_primary_failed;
	atomic64_t set_led_locate_failed;
	atomic64_t set_led_fault_failed;
	atomic64_t set_led_readback_failed;
	atomic64_t set_led_failed;
	atomic64_t get_led_failed;
	atomic64_t completion_timeout;
	atomic64_t wrong_msgid;
	atomic64_t request_failed;
};

static ssize_t craye1k_show(struct kobject *kobj, struct kobj_attribute *kattr,
			    char *buf);
static ssize_t craye1k_store(struct kobject *kobj, struct kobj_attribute *kattr,
			     const char *buf,
			     size_t count);
static void craye1k_new_smi(int iface, struct device *dev);
static void craye1k_smi_gone(int iface);
static void craye1k_msg_handler(struct ipmi_recv_msg *msg, void *user_msg_data);

struct craye1k_driver_data {
	struct ipmi_smi_watcher smi_watcher;
	struct ipmi_user_hndl user_hndl;
	struct craye1k *craye1k;
};

static struct craye1k_driver_data craye1k_driver_data = {
	.smi_watcher = {
		.owner = THIS_MODULE,
		.new_smi = craye1k_new_smi,
		.smi_gone = craye1k_smi_gone
	},
	.user_hndl = {
		.ipmi_recv_hndl = craye1k_msg_handler,
	},
	.craye1k = NULL,
};

static const struct kobj_attribute craye1k_kattr[] = {
	__ATTR(check_primary, 0660, craye1k_show, craye1k_store),
	__ATTR(check_primary_failed, 0660, craye1k_show, craye1k_store),
	__ATTR(was_already_primary, 0660, craye1k_show, craye1k_store),
	__ATTR(was_not_already_primary, 0660, craye1k_show, craye1k_store),
	__ATTR(set_primary, 0660, craye1k_show, craye1k_store),
	__ATTR(set_initial_primary_failed, 0660, craye1k_show, craye1k_store),
	__ATTR(set_primary_failed, 0660, craye1k_show, craye1k_store),
	__ATTR(set_led_locate_failed, 0660, craye1k_show, craye1k_store),
	__ATTR(set_led_fault_failed, 0660, craye1k_show, craye1k_store),
	__ATTR(set_led_readback_failed, 0660, craye1k_show, craye1k_store),
	__ATTR(set_led_failed, 0660, craye1k_show, craye1k_store),
	__ATTR(get_led_failed, 0660, craye1k_show, craye1k_store),
	__ATTR(completion_timeout, 0660, craye1k_show, craye1k_store),
	__ATTR(wrong_msgid, 0660, craye1k_show, craye1k_store),
	__ATTR(request_failed, 0660, craye1k_show, craye1k_store),
};

/*
 * The individual craye1k_attrs[] entries get initialized in
 * craye1k_create_sysfs.
 *
 * The last craye1k_attrs[] entry is for the NULL terminator.
 */
static struct attribute *craye1k_kattrs[ARRAY_SIZE(craye1k_kattr) + 1] = {0};
static struct attribute_group craye1k_group = {
	.attrs = craye1k_kattrs,
	.name = "craye1k_stats"
};

#define CRAYE1K_TABLE(_name) { \
	.name = __stringify(_name), \
	.offset = offsetof(struct craye1k, _name) \
}

/* Given a kobj_attribute for the stat, return a pointer to its stat value */
static atomic64_t *craye1k_lookup_stat(struct kobject *kobj, const char *name)
{
	struct craye1k *craye1k;
	struct device *dev;
	int i;

	/* Lookup table for name -> atomic64_t offset */
	const struct {
		const char *name;
		size_t offset;
	} table[] = {
		CRAYE1K_TABLE(check_primary),
		CRAYE1K_TABLE(check_primary_failed),
		CRAYE1K_TABLE(was_already_primary),
		CRAYE1K_TABLE(was_not_already_primary),
		CRAYE1K_TABLE(set_primary),
		CRAYE1K_TABLE(set_initial_primary_failed),
		CRAYE1K_TABLE(set_primary_failed),
		CRAYE1K_TABLE(set_led_locate_failed),
		CRAYE1K_TABLE(set_led_fault_failed),
		CRAYE1K_TABLE(set_led_readback_failed),
		CRAYE1K_TABLE(set_led_failed),
		CRAYE1K_TABLE(get_led_failed),
		CRAYE1K_TABLE(completion_timeout),
		CRAYE1K_TABLE(wrong_msgid),
		CRAYE1K_TABLE(request_failed)
	};

	dev = container_of(kobj, struct device, kobj);
	craye1k = dev_get_drvdata(dev);

	/* lookup atomic64_t from name */
	for (i = 0; i < ARRAY_SIZE(table); i++)
		if (strcmp(table[i].name, name) == 0)
			return (atomic64_t *)((char *)craye1k + table[i].offset);

	return NULL;
}

static int craye1k_create_sysfs(struct craye1k *craye1k)
{
	int i;
	atomic64_t *a;

	/*
	 * Fill in craye1k_kattrs[] with the pointers to our kobj_attributes.
	 */
	for (i = 0; i < ARRAY_SIZE(craye1k_kattrs) - 1; i++) {
		craye1k_kattrs[i] = (struct attribute *)&craye1k_kattr[i].attr;

		/* For completeness, initialize all atomics to 0 */
		a = craye1k_lookup_stat(&craye1k->dev->kobj,
					craye1k_kattr[i].attr.name);
		atomic64_set(a, 0);
	}

	craye1k_kattrs[i] = NULL;	/* mark end of the list */

	return sysfs_create_group(&craye1k->dev->kobj, &craye1k_group);
}

static void craye1k_remove_sysfs(struct craye1k *craye1k)
{
	sysfs_remove_group(&craye1k->dev->kobj, &craye1k_group);
}

static ssize_t craye1k_show(struct kobject *kobj, struct kobj_attribute *kattr,
			    char *buf)
{
	atomic64_t *val;

	val = craye1k_lookup_stat(kobj, kattr->attr.name);
	if (!val)
		return -EINVAL;

	return sprintf(buf, "%llu\n", atomic64_read(val));
}

/* A write of anything clears counter */
static ssize_t craye1k_store(struct kobject *kobj, struct kobj_attribute *kattr,
			     const char *buf, size_t count)
{
	atomic64_t *val;

	val = craye1k_lookup_stat(kobj, kattr->attr.name);
	if (!val)
		return -EINVAL;

	atomic64_set(val, 0);

	return count;
}

/*
 * craye1k_msg_handler() - IPMI message response handler
 */
static void craye1k_msg_handler(struct ipmi_recv_msg *msg, void *user_msg_data)
{
	struct craye1k *craye1k = user_msg_data;

	WARN_ON(craye1k != craye1k_driver_data.craye1k);

	if (msg->msgid != craye1k->tx_msg_id) {
		atomic64_inc(&craye1k->wrong_msgid);
		if (print_errors) {
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
				  0, ipmi_retries, ipmi_timeout);
	if (rc) {
		atomic64_inc(&craye1k->request_failed);
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
	unsigned long tout = msecs_to_jiffies(completion_timeout);

	rc = craye1k_send_message(craye1k);
	if (rc == 0) {
		rc2 = wait_for_completion_killable_timeout(read_complete, tout);
		if (rc2 == 0) {
			atomic64_inc(&craye1k->completion_timeout);
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
 * Returns: the last byte from the response or 0 if response had no response
 * data bytes, else -1 on error.
 */
static int craye1k_do_command(struct craye1k *craye1k, u8 cmd, u8 *send_data,
			      u8 send_data_len)
{
	int rc;

	if (mutex_lock_interruptible(&craye1k->lock) != 0)
		return -1;

	rc = craye1k_do_command_and_netfn(craye1k, CRAYE1K_CMD_NETFN, cmd,
					  send_data, send_data_len);

	mutex_unlock(&craye1k->lock);

	return rc;
}

/*
 * __craye1k_set_primary() - Tell the BMC we want to be the primary server
 *
 * An E1000 board has two physical servers on it.  In order to set a slot
 * NVMe LED, this server needs to first tell the BMC that it's the primary
 * server.
 *
 * Returns: 0 on success, 1 otherwise.
 */

static int __craye1k_set_primary(struct craye1k *craye1k)
{
	u8 bytes[2] = {CRAYE1K_SET_PRIMARY, 1};	/* set primary to 1 */
	int rc;

	atomic64_inc(&craye1k->set_primary);
	rc = craye1k_do_command(craye1k, CRAYE1K_CMD_PRIMARY, bytes, 2);
	if (rc == -1)
		return 1;

	return 0;
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
	atomic64_inc(&craye1k->check_primary);
	if (rc == 0x1)
		return 1;   /* success */

	atomic64_inc(&craye1k->check_primary_failed);
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
		atomic64_inc(&craye1k->was_already_primary);
		return 0;
	}
	atomic64_inc(&craye1k->was_not_already_primary);

	if (__craye1k_set_primary(craye1k) != 0) {
		atomic64_inc(&craye1k->set_initial_primary_failed);
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
		atomic64_inc(&craye1k->set_primary_failed);
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
 * Returns: 0 on success, 1 on failure.
 */
static int craye1k_set_slot_led(struct craye1k *craye1k, unsigned char slot,
				unsigned char is_locate_led,
				unsigned char value)
{
	int rc;
	u8 bytes[3];

	bytes[0] = CRAYE1K_SET_LED;
	bytes[1] = slot;
	bytes[2] = value;

	if (is_locate_led)
		rc = craye1k_do_command(craye1k, CRAYE1K_CMD_LOCATE_LED, bytes,
					3);
	else
		rc = craye1k_do_command(craye1k, CRAYE1K_CMD_FAULT_LED, bytes, 3);

	if (rc == -1) {
		/* Error setting LED - let higher level retries deal with it */
		return 1;
	}

	return 0;
}

static struct craye1k *craye1k_from_hotplug_slot(struct hotplug_slot
						 *hotplug_slot)
{
	unsigned char slot;
	const struct hotplug_slot_ops *ops;
	struct craye1k *craye1k;

	slot = PSN(to_ctrl(hotplug_slot));

	/*
	 * We know that our attention status callback functions have been swapped
	 * into the PCI device's hotplug_slot->ops values.  We can use that
	 * knowledge to lookup our craye1k.
	 *
	 * To do that, we use the current hotplug_slot->ops value, which is going
	 * to be one of the entries in craye1k->ops[], and offset our slot number
	 * to get the address of craye1k->ops[0].  We then use that with
	 * container_of() to get craye1k.  Slots start at 1, so account for that.
	 */
	ops = hotplug_slot->ops + 1 - slot;
	craye1k = container_of(ops, struct craye1k, new_ops[0]);

	return craye1k;
}

static int __craye1k_get_attention_status(struct hotplug_slot *hotplug_slot,
					  u8 *status, bool set_primary)
{
	unsigned char slot;
	int locate, fault;
	int rc = 0;
	struct craye1k *craye1k;

	slot = PSN(to_ctrl(hotplug_slot));
	if (!(slot >= 1 && slot <= 24)) {
		rc = -EINVAL;
		goto out;
	}

	craye1k = craye1k_from_hotplug_slot(hotplug_slot);

	if (set_primary) {
		if (craye1k_set_primary(craye1k) != 0) {
			rc = -EIO;
			goto out;
		}
	}

	locate = craye1k_get_slot_led(craye1k, slot, true);
	if (locate == -1) {
		rc = -EINVAL;
		goto out;
	}

	fault = craye1k_get_slot_led(craye1k, slot, false);
	if (fault == -1) {
		rc = -EINVAL;
		goto out;
	}

	if (rc != 0)
		atomic64_inc(&craye1k->get_led_failed);

	*status = locate << 1 | fault;

out:
	return rc;
}

static int craye1k_get_attention_status(struct hotplug_slot *hotplug_slot,
					u8 *status)
{
	return __craye1k_get_attention_status(hotplug_slot, status, true);
}

static int craye1k_set_attention_status(struct hotplug_slot *hotplug_slot,
					u8 status)
{
	unsigned char slot;
	int tries = 4;
	int rc;
	u8 new_status;
	struct craye1k *craye1k;
	bool locate, fault;

	slot = PSN(to_ctrl(hotplug_slot));
	if (!(slot >= 1 && slot <= 24))
		return -EINVAL;

	craye1k = craye1k_from_hotplug_slot(hotplug_slot);

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

		/* locate */
		locate = (status & 0x2) >> 1;
		if (craye1k_set_slot_led(craye1k, slot, 1, locate) != 0) {
			atomic64_inc(&craye1k->set_led_locate_failed);
			continue;	/* fail, retry */
		}

		/* fault */
		fault = status & 0x1;
		if (craye1k_set_slot_led(craye1k, slot, 0, fault) != 0) {
			atomic64_inc(&craye1k->set_led_fault_failed);
			continue;	/* fail, retry */
		}

		rc = __craye1k_get_attention_status(hotplug_slot, &new_status,
						    false);
		if (rc == 0 && new_status == status)
			break;	/* success */

		atomic64_inc(&craye1k->set_led_readback_failed);

		/*
		 * At this point we weren't successful in setting the LED and
		 * need to try again.
		 *
		 * Do a random back-off to reduce contention with other server
		 * node in the unlikely case that both server nodes are trying to
		 * trying to set a LED at the same time.
		 *
		 * The 500ms minimum in the backoff reduced the chance of this
		 * whole retry loop failing from 1 in 700 to none in 10000.
		 */
		msleep(500 + (get_random_long() % 500));
	}

	if (tries == 0) {
		atomic64_inc(&craye1k->set_led_failed);
		return -EIO;
	}

	return 0;
}

/*
 * Returns the hotplug controller for a given pci_dev (if any).
 */
static struct controller *craye1k_pci_dev_to_ctrl(struct pci_dev *dev)
{
	struct device *device;
	struct pcie_device *edev;
	struct controller *ctrl;

	device = pcie_port_find_device(dev, PCIE_PORT_SERVICE_HP);
	if (!device)
		return NULL;

	edev = to_pcie_device(device);
	if (!edev)
		return NULL;

	ctrl = get_service_data(edev);
	if (!ctrl)
		return NULL;

	return ctrl;
}

/*
 * Update the hotplug 'attention' callbacks to point to craye1k's callbacks.
 */
static void craye1k_setup_attention_callbacks(struct craye1k *craye1k)
{
	struct pci_dev *dev = NULL;
	const struct hotplug_slot_ops *ops;
	struct hotplug_slot_ops *new_ops;
	struct controller *ctrl;
	unsigned char slot;

	/*
	 * Iterate though all the PCI devices looking for the ones controlled
	 * by the pciehp driver.
	 */
	for_each_pci_dev(dev) {
		ctrl = craye1k_pci_dev_to_ctrl(dev);
		if (!ctrl)	/* not controlled by pciehp */
			continue;

		/* craye1k slots are numbered 1-24 */
		slot = PSN(ctrl);
		if (!(slot >= 1 && slot <= 24))
			continue;

		mutex_lock(&ctrl->state_lock);
		/*
		 * Save old hotplug ops callbacks for restoration when
		 * we unload the driver.
		 *
		 * Note: the craye1k slots are numbered starting at 1, not 0.
		 */
		ops = ctrl->hotplug_slot.ops;
		craye1k->orig_ops[slot - 1] = ops;

		/*
		 * 'ops' is const, so we can't just go in and change
		 * ctrl->hotplug_slot.ops.[get|set]_attention_status to
		 * point to our callbacks.  Instead we make a copy of ops,
		 * update our callbacks in it, and point ctrl->hotplug_slot.ops
		 * to our new 'ops'.
		 */
		new_ops = &craye1k->new_ops[slot - 1];
		memcpy(new_ops, ops, sizeof(*ops));
		mutex_unlock(&ctrl->state_lock);

		new_ops->set_attention_status = craye1k_set_attention_status;
		new_ops->get_attention_status = craye1k_get_attention_status;

		/*
		 * Re-generate sysfs entry for our ops.  In this case, it will
		 * add our 'attention' sysfs entry for slots 1-24.
		 */
		pci_hp_del(&ctrl->hotplug_slot);

		mutex_lock(&ctrl->state_lock);
		ctrl->hotplug_slot.ops = new_ops;
		mutex_unlock(&ctrl->state_lock);

		pci_hp_add(&ctrl->hotplug_slot);
	}
}

static void craye1k_restore_attention_callbacks(struct craye1k *craye1k)
{
	struct pci_dev *dev = NULL;
	unsigned char slot;
	struct controller *ctrl;

	for_each_pci_dev(dev) {
		ctrl = craye1k_pci_dev_to_ctrl(dev);
		if (!ctrl)
			continue;

		slot = PSN(ctrl);
		if (!(slot >= 1 && slot <= 24))
			continue;

		pci_hp_del(&ctrl->hotplug_slot);

		mutex_lock(&ctrl->state_lock);
		ctrl->hotplug_slot.ops = craye1k->orig_ops[slot - 1];
		mutex_unlock(&ctrl->state_lock);

		pci_hp_add(&ctrl->hotplug_slot);
	}
}

static void craye1k_new_smi(int iface, struct device *dev)
{
	int rc;
	struct craye1k *craye1k;

	WARN_ON(craye1k_driver_data.craye1k);

	craye1k = kzalloc(sizeof(*craye1k), GFP_KERNEL);
	if (!craye1k)
		return;

	craye1k->address.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
	craye1k->address.channel = IPMI_BMC_CHANNEL;
	craye1k->iface = iface;
	craye1k->dev = dev;
	craye1k->tx_msg.data = craye1k->tx_msg_data;
	init_completion(&craye1k->read_complete);
	mutex_init(&craye1k->lock);

	dev_set_drvdata(dev, craye1k);
	craye1k_driver_data.craye1k = craye1k;

	rc = ipmi_create_user(craye1k->iface, &craye1k_driver_data.user_hndl,
			      craye1k, &craye1k->user);
	if (rc < 0) {
		dev_err_ratelimited(dev,
				    "Unable to register IPMI user, iface %d\n",
				    craye1k->iface);
		kfree(craye1k);
		craye1k = NULL;
	} else {
		craye1k_setup_attention_callbacks(craye1k);
	}
	if (craye1k_create_sysfs(craye1k) != 0)
		dev_err_ratelimited(dev, "Couldn't create sysfs entries");

	dev_info_ratelimited(dev, "Cray ClusterStor E1000 slot LEDs registered");
}

static void craye1k_cleanup(struct craye1k *craye1k)
{
	if (craye1k) {
		craye1k_remove_sysfs(craye1k);
		craye1k_restore_attention_callbacks(craye1k);
		ipmi_destroy_user(craye1k->user);
		craye1k_driver_data.craye1k = NULL;
		kfree(craye1k);
	}
}

static void craye1k_smi_gone(int iface)
{
	struct craye1k *craye1k = craye1k_driver_data.craye1k;

	if (craye1k->iface != iface)
		return;

	craye1k_cleanup(craye1k);
}

static bool is_craye1k_board(void)
{
	return dmi_match(DMI_PRODUCT_NAME, "VSSEP1EC");
}

static int craye1k_init(void)
{
	if (!is_craye1k_board())
		return 0;

	return ipmi_smi_watcher_register(&craye1k_driver_data.smi_watcher);
}

static void craye1k_exit(void)
{
	if (!is_craye1k_board())
		return;

	ipmi_smi_watcher_unregister(&craye1k_driver_data.smi_watcher);
	craye1k_cleanup(craye1k_driver_data.craye1k);
}

MODULE_SOFTDEP("pre: pciehp");

module_init(craye1k_init);
module_exit(craye1k_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tony Hutter <hutter2@llnl.gov>");
MODULE_DESCRIPTION("Cray E1000 NVMe Slot LED driver");
