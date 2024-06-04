// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2024 Intel Corporation. */

#include "iavf.h"

/**
 * clock_to_adapter - Convert clock info pointer to adapter pointer
 * @ptp_info: PTP info structure
 *
 * Use container_of in order to extract a pointer to the iAVF adapter private
 * structure.
 *
 * Return: pointer to iavf_adapter structure
 */
static struct iavf_adapter *clock_to_adapter(struct ptp_clock_info *ptp_info)
{
	struct iavf_ptp *ptp_priv;

	ptp_priv = container_of(ptp_info, struct iavf_ptp, info);
	return container_of(ptp_priv, struct iavf_adapter, ptp);
}

/**
 * iavf_ptp_cap_supported - Check if a PTP capability is supported
 * @adapter: private adapter structure
 * @cap: the capability bitmask to check
 *
 * Return: true if every capability set in cap is also set in the enabled
 *         capabilities reported by the PF, false otherwise.
 */
bool iavf_ptp_cap_supported(struct iavf_adapter *adapter, u32 cap)
{
	if (!PTP_ALLOWED(adapter))
		return false;

	/* Only return true if every bit in cap is set in hw_caps.caps */
	return (adapter->ptp.hw_caps.caps & cap) == cap;
}

/**
 * iavf_allocate_ptp_cmd - Allocate a PTP command message structure
 * @v_opcode: the virtchnl opcode
 * @msglen: length in bytes of the associated virtchnl structure
 *
 * Allocates a PTP command message and pre-fills it with the provided message
 * length and opcode.
 *
 * Return: allocated PTP command
 */
static struct iavf_ptp_aq_cmd *iavf_allocate_ptp_cmd(enum virtchnl_ops v_opcode,
						     u16 msglen)
{
	struct iavf_ptp_aq_cmd *cmd;

	cmd = kzalloc(struct_size(cmd, msg, msglen), GFP_KERNEL);
	if (!cmd)
		return NULL;

	cmd->v_opcode = v_opcode;
	cmd->msglen = msglen;

	return cmd;
}

/**
 * iavf_queue_ptp_cmd - Queue PTP command for sending over virtchnl
 * @adapter: private adapter structure
 * @cmd: the command structure to send
 *
 * Queue the given command structure into the PTP virtchnl command queue tos
 * end to the PF.
 */
static void iavf_queue_ptp_cmd(struct iavf_adapter *adapter,
			       struct iavf_ptp_aq_cmd *cmd)
{
	spin_lock(&adapter->ptp.aq_cmd_lock);
	list_add_tail(&cmd->list, &adapter->ptp.aq_cmds);
	spin_unlock(&adapter->ptp.aq_cmd_lock);

	adapter->aq_required |= IAVF_FLAG_AQ_SEND_PTP_CMD;
	mod_delayed_work(adapter->wq, &adapter->watchdog_task, 0);
}

/**
 * iavf_send_phc_read - Send request to read PHC time
 * @adapter: private adapter structure
 *
 * Send a request to obtain the PTP hardware clock time. This allocates the
 * VIRTCHNL_OP_1588_PTP_GET_TIME message and queues it up to send to
 * indirectly read the PHC time.
 *
 * This function does not wait for the reply from the PF.
 *
 * Return: 0 if success, error code otherwise
 */
static int iavf_send_phc_read(struct iavf_adapter *adapter)
{
	struct iavf_ptp_aq_cmd *cmd;

	if (!adapter->ptp.initialized)
		return -EOPNOTSUPP;

	cmd = iavf_allocate_ptp_cmd(VIRTCHNL_OP_1588_PTP_GET_TIME,
				    sizeof(struct virtchnl_phc_time));
	if (!cmd)
		return -ENOMEM;

	iavf_queue_ptp_cmd(adapter, cmd);

	return 0;
}

/**
 * iavf_read_phc_indirect - Indirectly read the PHC time via virtchnl
 * @adapter: private adapter structure
 * @ts: storage for the timestamp value
 * @sts: system timestamp values before and after the read
 *
 * Used when the device does not have direct register access to the PHC time.
 * Indirectly reads the time via the VIRTCHNL_OP_1588_PTP_GET_TIME, and waits
 * for the reply from the PF.
 *
 * Based on some simple measurements using ftrace and phc2sys, this clock
 * access method has about a ~110 usec latency even when the system is not
 * under load. In order to achieve acceptable results when using phc2sys with
 * the indirect clock access method, it is recommended to use more
 * conservative proportional and integration constants with the P/I servo.
 *
 * Return: 0 if success, error code otherwise
 */
static int iavf_read_phc_indirect(struct iavf_adapter *adapter,
				  struct timespec64 *ts,
				  struct ptp_system_timestamp *sts)
{
	long ret;
	int err;

	adapter->ptp.phc_time_ready = false;
	ptp_read_system_prets(sts);

	err = iavf_send_phc_read(adapter);
	if (err)
		return err;

	ret = wait_event_interruptible_timeout(adapter->ptp.phc_time_waitqueue,
					       adapter->ptp.phc_time_ready,
					       HZ);
	if (ret < 0)
		return ret;
	else if (!ret)
		return -EBUSY;

	*ts = ns_to_timespec64(adapter->ptp.cached_phc_time);

	ptp_read_system_postts(sts);

	return 0;
}

static int iavf_ptp_gettimex64(struct ptp_clock_info *ptp,
			       struct timespec64 *ts,
			       struct ptp_system_timestamp *sts)
{
	struct iavf_adapter *adapter = clock_to_adapter(ptp);

	if (!adapter->ptp.initialized)
		return -ENODEV;

	return iavf_read_phc_indirect(adapter, ts, sts);
}

/**
 * iavf_ptp_register_clock - Register a new PTP for userspace
 * @adapter: private adapter structure
 *
 * Allocate and register a new PTP clock device if necessary.
 *
 * Return: 0 if success, error otherwise
 */
static int iavf_ptp_register_clock(struct iavf_adapter *adapter)
{
	struct ptp_clock_info *ptp_info = &adapter->ptp.info;
	struct device *dev = &adapter->pdev->dev;

	memset(ptp_info, 0, sizeof(*ptp_info));

	snprintf(ptp_info->name, sizeof(ptp_info->name) - 1, "%s-%s-clk",
		 dev_driver_string(dev),
		 dev_name(dev));
	ptp_info->owner = THIS_MODULE;
	ptp_info->gettimex64 = iavf_ptp_gettimex64;

	adapter->ptp.clock = ptp_clock_register(ptp_info, dev);
	if (IS_ERR(adapter->ptp.clock))
		return PTR_ERR(adapter->ptp.clock);

	dev_info(&adapter->pdev->dev, "PTP clock %s registered\n",
		 adapter->ptp.info.name);
	return 0;
}

/**
 * iavf_ptp_init - Initialize PTP support if capability was negotiated
 * @adapter: private adapter structure
 *
 * Initialize PTP functionality, based on the capabilities that the PF has
 * enabled for this VF.
 */
void iavf_ptp_init(struct iavf_adapter *adapter)
{
	struct device *dev = &adapter->pdev->dev;
	int err;

	if (WARN_ON(adapter->ptp.initialized)) {
		dev_err(dev, "PTP functionality was already initialized!\n");
		return;
	}

	if (!iavf_ptp_cap_supported(adapter, VIRTCHNL_1588_PTP_CAP_READ_PHC)) {
		dev_dbg(dev, "Device does not have PTP clock support\n");
		return;
	}

	err = iavf_ptp_register_clock(adapter);
	if (err) {
		dev_warn(dev, "Failed to register PTP clock device (%d)\n",
			 err);
		return;
	}

	adapter->ptp.initialized = true;
}

/**
 * iavf_ptp_release - Disable PTP support
 * @adapter: private adapter structure
 *
 * Release all PTP resources that were previously initialized.
 */
void iavf_ptp_release(struct iavf_adapter *adapter)
{
	struct iavf_ptp_aq_cmd *cmd, *tmp;

	adapter->ptp.initialized = false;

	if (!IS_ERR_OR_NULL(adapter->ptp.clock)) {
		dev_info(&adapter->pdev->dev, "removing PTP clock %s\n",
			 adapter->ptp.info.name);
		ptp_clock_unregister(adapter->ptp.clock);
		adapter->ptp.clock = NULL;
	}

	/* Cancel any remaining uncompleted PTP clock commands */
	spin_lock(&adapter->ptp.aq_cmd_lock);
	list_for_each_entry_safe(cmd, tmp, &adapter->ptp.aq_cmds, list) {
		list_del(&cmd->list);
		kfree(cmd);
	}
	adapter->aq_required &= ~IAVF_FLAG_AQ_SEND_PTP_CMD;
	spin_unlock(&adapter->ptp.aq_cmd_lock);
}

/**
 * iavf_ptp_process_caps - Handle change in PTP capabilities
 * @adapter: private adapter structure
 *
 * Handle any state changes necessary due to change in PTP capabilities, such
 * as after a device reset or change in configuration from the PF.
 */
void iavf_ptp_process_caps(struct iavf_adapter *adapter)
{
	struct device *dev = &adapter->pdev->dev;

	dev_dbg(dev, "PTP capabilities changed at runtime\n");

	/* Check if the device gained or lost necessary access to support the
	 * PTP hardware clock. If so, driver must respond appropriately by
	 * creating or destroying the PTP clock device.
	 */
	if (adapter->ptp.initialized &&
	    !iavf_ptp_cap_supported(adapter, VIRTCHNL_1588_PTP_CAP_READ_PHC))
		iavf_ptp_release(adapter);
	else if (!adapter->ptp.initialized &&
		 iavf_ptp_cap_supported(adapter, VIRTCHNL_1588_PTP_CAP_READ_PHC))
		iavf_ptp_init(adapter);
}
