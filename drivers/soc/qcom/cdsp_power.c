// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * CDSP Power Management Driver for IPQ9650
 *
 * Regulator ownership hierarchy:
 *   PAS remoteproc (cx-supply / mx-supply)
 *       -> cdsp-vdd-cx / cdsp-vdd-mx  (virtual, provided by this driver)
 *           -> vdd-cx / vdd-mx        (voltage regulators, consumed by this driver)
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/soc/qcom/smem.h>
#include <linux/workqueue.h>

/* SMEM Protocol Definitions */
/* NSP Q6 SMEM host ID */
#define CDSP_SMEM_NSP_HOST_ID	5
/* hdr(64) + request_area(1024) + response_area(256) */
#define CDSP_SMEM_SIZE		1344
/* Protocol magic number 'RPMH' */
#define CDSP_SMEM_MAGIC		0x52504D48
/* Protocol version v1.0 */
#define CDSP_SMEM_VERSION	0x00010001
#define CDSP_MSG_ID_REQUEST	0x01
#define CDSP_MSG_ID_RESPONSE	0x02
#define CDSP_RESP_MSG_SIZE	24
#define CDSP_MAX_KVP		64

/* SMEM layout fixed offsets (relative to start of SMEM item) */
#define CDSP_SMEM_REQUEST_OFFSET	64
#define CDSP_SMEM_REQUEST_SIZE		1024
#define CDSP_SMEM_RESPONSE_OFFSET	1088
#define CDSP_SMEM_RESPONSE_SIZE		256

/* NSP CX voltage rail resource ID */
#define CDSP_RESOURCE_ID_CX	0x03
/* NSP MX voltage rail resource ID */
#define CDSP_RESOURCE_ID_MX	0x04

/* MPM Register Offsets */
#define RSC_HDSHK_IRQ_STAT		0x0004
#define CLIENT_RSC_HDSHK(n)		(0x0010 + (n) * 0x10)
#define CLIENT_RSC_IRQ_STAT(n)		(0x0014 + (n) * 0x10)
#define CLIENT_RSC_IRQ_CLR(n)		(0x0018 + (n) * 0x10)
#define VDD_RAIL_ISO_CTRL(n)		(0x0330 + (n) * 0x4)

/* RSCC Register Offsets */
#define RSCC_BR_EVENT			0x0
#define RSCC_BR_EVENT_PC_MODE		BIT(3)

/* MPM Handshake Bits */
#define MPM_SHUTDOWN_REQ		BIT(0)
#define MPM_SHUTDOWN_ACK		BIT(1)
#define MPM_BRINGUP_REQ			BIT(2)
#define MPM_BRINGUP_ACK			BIT(3)

/* MPM Isolation Control Bits */
#define ISO_CLK_DIS			BIT(0)
#define ISO_CLK_DIS_ACK			BIT(1)
#define ISO_INPUT			BIT(8)
#define ISO_INPUT_DFT			BIT(9)
#define ISO_INPUT_CLKS			BIT(10)
#define ISO_CLAMP_MEM			BIT(11)
#define ISO_RET_0PIN			BIT(16)
#define ISO_SAVE_FF			BIT(17)
#define ISO_RESTORE_FF			BIT(18)
#define ISO_FREEZE_OUTPUT		BIT(24)
#define ISO_PWR_UP_RESET		BIT(25)

/* MXC rail mapped to isolation control register index 0 */
#define VDD_RAIL_MX			0
/* NSP/CX rail mapped to isolation control register index 1 */
#define VDD_RAIL_CX			1

/* Power States */
#define CDSP_POWER_OFF			0
#define CDSP_POWER_ON			1

/* Timeouts */
#define MPM_POLL_TIMEOUT_US		10000
#define MPM_POLL_SLEEP_US		10

/* Virtual regulator IDs */
enum cdsp_virt_reg_id {
	CDSP_VIRT_NSP_CX = 0,
	CDSP_VIRT_NSP_MX = 1,
	CDSP_VIRT_MAX,
};

/**
 * struct cdsp_smem_channel_hdr - SMEM channel header (64 bytes, at offset 0x000)
 * @magic:             Protocol magic number (0x52504D48 = 'RPMH')
 * @version:           Protocol version (0x00010001 = v1.0)
 * @cdsp_state:        NSP Q6 readiness flag; 0 = not ready, 1 = ready (written by NSP)
 * @apss_state:        APSS readiness flag; 0 = not ready, 1 = ready (written by APSS)
 * @request_offset:    Byte offset of the request area from the start of the SMEM item
 * @request_size:      Size of the request area in bytes
 * @response_offset:   Byte offset of the response area from the start of the SMEM item
 * @response_size:     Size of the response area in bytes
 * @request_in_flight: Channel busy flag; 0 = idle, 1 = busy (NSP sets, APSS clears)
 * @cdsp_to_apss_irq:  IPCC signal ID used for NSP-to-APSS interrupts
 * @apss_to_cdsp_irq:  IPCC signal ID used for APSS-to-NSP interrupts
 * @cdsp_tx_count:     Number of DCVS requests sent by NSP
 * @cdsp_rx_count:     Number of DCVS responses received by NSP
 * @apss_tx_count:     Number of DCVS responses sent by APSS
 * @apss_rx_count:     Number of DCVS requests received by APSS
 * @reserved:          Reserved, must be zero
 *
 * Fixed at offset 0x000 in the SMEM item. Matches rpmh_smem_channel_header_t
 * on the NSP Q6 side. APSS initialises this at probe time; NSP Q6 reads it
 * to discover the request/response area offsets and to synchronise readiness.
 */
struct cdsp_smem_channel_hdr {
	u32 magic;
	u32 version;
	u32 cdsp_state;
	u32 apss_state;
	u32 request_offset;
	u32 request_size;
	u32 response_offset;
	u32 response_size;
	u32 request_in_flight;
	u32 cdsp_to_apss_irq;
	u32 apss_to_cdsp_irq;
	u32 cdsp_tx_count;
	u32 cdsp_rx_count;
	u32 apss_tx_count;
	u32 apss_rx_count;
	u32 reserved;
} __packed;

/**
 * struct cdsp_kvp - Key-Value-Pair entry in a DCVS request
 * @key:    Resource identifier; upper 16 bits = CDSP_RESOURCE_ID_CX or _MX
 * @length: Length of the value field in bytes (always 4)
 * @value:  Requested voltage in microvolts
 */
struct cdsp_kvp {
	u32 key;
	u32 length;
	u32 value;
} __packed;

/**
 * struct cdsp_smem_request - DCVS request message written by NSP Q6, read by APSS
 * @msg_size:     Total message size in bytes
 * @sequence:     Monotonically increasing sequence number
 * @msg_id:       Message type; CDSP_MSG_ID_REQUEST (0x01) for DCVS requests
 * @req_id:       Request identifier echoed in the response
 * @set:          Power set being requested (ACTIVE / SLEEP / WAKE)
 * @num_commands: Number of KVP entries that follow
 * @timestamp_us: Request timestamp in microseconds
 * @kvp:          Array of key-value-pair voltage commands (up to CDSP_MAX_KVP)
 */
struct cdsp_smem_request {
	u32 msg_size;
	u32 sequence;
	u32 msg_id;
	u32 req_id;
	u32 set;
	u32 num_commands;
	u32 timestamp_us;
	struct cdsp_kvp kvp[CDSP_MAX_KVP];
} __packed;

/**
 * struct cdsp_smem_response - DCVS response message written by APSS, read by NSP Q6
 * @msg_size:     Total message size in bytes (CDSP_RESP_MSG_SIZE = 24)
 * @sequence:     Echo of the request sequence number
 * @msg_id:       Message type; CDSP_MSG_ID_RESPONSE (0x02)
 * @status:       Result code; 0 on success, negative errno on failure
 * @data:         Actual voltage applied in microvolts (valid when status == 0)
 * @timestamp_us: Completion timestamp in microseconds
 */
struct cdsp_smem_response {
	u32 msg_size;
	u32 sequence;
	u32 msg_id;
	u32 status;
	u32 data;
	u32 timestamp_us;
} __packed;

/**
 * struct cdsp_smem_region - Full SMEM item layout (1344 bytes total)
 * @hdr:       Channel header at offset 0x000 (64 bytes)
 * @request:   DCVS request area at offset 0x040 (padded to 1024 bytes)
 * @_req_pad:  Padding to align response area to offset 0x440
 * @response:  DCVS response area at offset 0x440 (padded to 256 bytes)
 * @_resp_pad: Padding to complete the 256-byte response area
 *
 * Layout matches the rpmh_smem_channel_header_t offsets on the NSP Q6 side.
 */
struct cdsp_smem_region {
	struct cdsp_smem_channel_hdr hdr;
	struct cdsp_smem_request  request;
	u8 _req_pad[228];
	struct cdsp_smem_response response;
	u8 _resp_pad[232];
} __packed;

/**
 * struct cdsp_power_driver - Main driver context
 * @dev: Device pointer
 * @smem: Pointer to SMEM region
 * @dcvs_mbox_client: Mailbox client for DCVS response interrupt (APSS->NSP)
 * @dcvs_mbox_chan: Mailbox channel for DCVS response interrupt
 * @dcvs_irq: DCVS interrupt number (NSP->APSS IPCC PING)
 * @dcvs_work: Work structure for DCVS processing
 * @mpm_regmap: Regmap for MPM handshake registers
 * @lpm_irq: LPM interrupt number
 * @lpm_work: Work structure for LPM processing
 * @rscc_regmap: Regmap for RSCC power mode detection register
 * @vdd_cx: CX voltage regulator (consumer handle)
 * @vdd_mx: MX voltage regulator (consumer handle, NULL if absent on this board)
 * @power_state: Current NSP power state (CDSP_POWER_ON / CDSP_POWER_OFF)
 * @lock: Mutex serialising DCVS and LPM work
 */
struct cdsp_power_driver {
	struct device *dev;

	/* SMEM for DCVS */
	struct cdsp_smem_region *smem;

	/* Mbox for DCVS response (APSS -> NSP via IPCC PING) */
	struct mbox_client dcvs_mbox_client;
	struct mbox_chan   *dcvs_mbox_chan;

	int dcvs_irq;
	struct work_struct dcvs_work;

	/* MPM for LPM */
	struct regmap *mpm_regmap;
	int lpm_irq;
	struct workqueue_struct *lpm_wq;
	struct work_struct lpm_work;

	/* RSCC for power mode detection */
	struct regmap *rscc_regmap;

	/* PMIC regulator consumer handles */
	struct regulator *vdd_cx;
	struct regulator *vdd_mx;

	/* State tracking */
	atomic_t power_state;
	/* Mutex serialising DCVS and LPM work */
	struct mutex lock;
};

/**
 * cdsp_virt_reg_enable() - Enable a virtual NSP regulator
 * @rdev: Regulator device
 *
 * Passes the enable request through to the underlying PMIC consumer handle
 * (vdd_cx or vdd_mx). Returns 0 immediately if the MX rail is absent on
 * this board.
 *
 * Return: 0 on success, negative error code on failure
 */
static int cdsp_virt_reg_enable(struct regulator_dev *rdev)
{
	struct cdsp_power_driver *drv = rdev_get_drvdata(rdev);
	int id = rdev_get_id(rdev);
	struct regulator *reg = (id == CDSP_VIRT_NSP_CX) ? drv->vdd_cx : drv->vdd_mx;

	if (!reg)
		return 0;
	return regulator_enable(reg);
}

/**
 * cdsp_virt_reg_is_enabled() - Check if a virtual NSP regulator is enabled
 * @rdev: Regulator device
 *
 * Queries the enable state of the underlying PMIC consumer handle (vdd_cx
 * or vdd_mx). Returns 1 if the MX rail is absent on this board, since the
 * MX hardware rail is always on in that configuration.
 *
 * Return: 1 if enabled (or absent), 0 if disabled, negative error code on failure
 */
static int cdsp_virt_reg_is_enabled(struct regulator_dev *rdev)
{
	struct cdsp_power_driver *drv = rdev_get_drvdata(rdev);
	int id = rdev_get_id(rdev);
	struct regulator *reg = (id == CDSP_VIRT_NSP_CX) ? drv->vdd_cx : drv->vdd_mx;

	if (!reg)
		return 1;
	return regulator_is_enabled(reg);
}

/**
 * cdsp_virt_reg_disable() - Disable a virtual NSP regulator
 * @rdev: Regulator device
 *
 * Passes the disable request through to the underlying PMIC consumer handle
 * (vdd_cx or vdd_mx). Returns 0 immediately if the MX rail is absent on
 * this board.
 *
 * Return: 0 on success, negative error code on failure
 */
static int cdsp_virt_reg_disable(struct regulator_dev *rdev)
{
	struct cdsp_power_driver *drv = rdev_get_drvdata(rdev);
	int id = rdev_get_id(rdev);
	struct regulator *reg = (id == CDSP_VIRT_NSP_CX) ? drv->vdd_cx : drv->vdd_mx;

	if (!reg)
		return 0;

	/* Disable the regulator if it's enabled */
	if (cdsp_virt_reg_is_enabled(rdev))
		return regulator_disable(reg);

	return 0;
}

static const struct regulator_ops cdsp_virt_reg_ops = {
	.enable      = cdsp_virt_reg_enable,
	.disable     = cdsp_virt_reg_disable,
	.is_enabled  = cdsp_virt_reg_is_enabled,
};

static const struct regulator_desc cdsp_virt_reg_descs[CDSP_VIRT_MAX] = {
	[CDSP_VIRT_NSP_CX] = {
		.id              = CDSP_VIRT_NSP_CX,
		.name            = "cdsp-vdd-cx",
		.of_match        = "cdsp-vdd-cx",
		.regulators_node = "regulators",
		.ops             = &cdsp_virt_reg_ops,
		.type            = REGULATOR_VOLTAGE,
		.owner           = THIS_MODULE,
	},
	[CDSP_VIRT_NSP_MX] = {
		.id              = CDSP_VIRT_NSP_MX,
		.name            = "cdsp-vdd-mx",
		.of_match        = "cdsp-vdd-mx",
		.regulators_node = "regulators",
		.ops             = &cdsp_virt_reg_ops,
		.type            = REGULATOR_VOLTAGE,
		.owner           = THIS_MODULE,
	},
};

/**
 * cdsp_is_full_pc_mode() - Check if FULL_PC mode is selected
 * @drv: Driver context
 *
 * Reads the CDSP RSCC_BR_EVENT register to determine power collapse mode.
 * Bit 3: 0 = FULL_PC (collapse both CX and MX)
 *        1 = LONG_APCR (collapse only CX)
 *
 * Return: true if FULL_PC mode, false if LONG_APCR mode
 */
static bool cdsp_is_full_pc_mode(struct cdsp_power_driver *drv)
{
	u32 br_event;
	int ret;

	ret = regmap_read(drv->rscc_regmap, RSCC_BR_EVENT, &br_event);
	if (ret) {
		dev_err(drv->dev, "Failed to read RSCC_BR_EVENT: %d\n", ret);
		return true; /* Default to FULL_PC on error */
	}

	/* Bit 3: 0 = FULL_PC (collapse both CX and MX)
	 *        1 = LONG_APCR (collapse only CX)
	 */
	return !(br_event & RSCC_BR_EVENT_PC_MODE);
}

/**
 * cdsp_dcvs_irq_handler() - DCVS interrupt handler
 * @irq: Interrupt number
 * @data: Driver context
 *
 * Called when CDSP sends a DCVS request via IPCC PING interrupt.
 * Schedules work to process the request.
 *
 * Return: IRQ_HANDLED
 */
static irqreturn_t cdsp_dcvs_irq_handler(int irq, void *data)
{
	struct cdsp_power_driver *drv = data;

	/* Schedule work to process DCVS request */
	schedule_work(&drv->dcvs_work);

	return IRQ_HANDLED;
}

/**
 * cdsp_dcvs_work_fn() - Process a DCVS voltage scaling request from the NSP Q6
 * @work: Work structure embedded in struct cdsp_power_driver
 *
 * Reads the KVP request from the SMEM channel, validates the resource ID and
 * voltage, applies the voltage via the regulator framework, writes the response
 * back to SMEM, and signals the NSP Q6 via the IPCC PING mailbox channel.
 */
static void cdsp_dcvs_work_fn(struct work_struct *work)
{
	struct cdsp_power_driver *drv = container_of(work,
						     struct cdsp_power_driver,
						     dcvs_work);
	struct cdsp_smem_region *smem = drv->smem;
	u32 sequence, num_commands;
	int ret = 0, actual_uv = 0;
	int i;

	mutex_lock(&drv->lock);

	/* Drop DCVS requests while the NSP Q6 is powered off */
	if (atomic_read(&drv->power_state) == CDSP_POWER_OFF) {
		dev_warn(drv->dev, "DCVS request while powered off, dropping\n");
		mutex_unlock(&drv->lock);
		return;
	}

	/* Read request header from SMEM */
	rmb();
	sequence     = smem->request.sequence;
	num_commands = smem->request.num_commands;

	/* Validate message type — only process DCVS requests */
	if (smem->request.msg_id != CDSP_MSG_ID_REQUEST) {
		dev_err(drv->dev, "Unexpected msg_id: 0x%x (expected 0x%x)\n",
			smem->request.msg_id, CDSP_MSG_ID_REQUEST);
		ret = -EINVAL;
		goto send_response;
	}

	dev_dbg(drv->dev, "DCVS request: seq=%u, num_commands=%u, timestamp=%u us\n",
		sequence, num_commands, smem->request.timestamp_us);

	if (num_commands > CDSP_MAX_KVP) {
		dev_err(drv->dev, "Too many KVP commands: %u (max %d)\n",
			num_commands, CDSP_MAX_KVP);
		ret = -EINVAL;
		goto send_response;
	}

	/* Process each KVP: key=resource_type, value=voltage_uv */
	for (i = 0; i < num_commands; i++) {
		u32 key        = smem->request.kvp[i].key;
		u32 voltage_uv = smem->request.kvp[i].value;
		/* Upper 16 bits of the KVP key encode the resource ID */
		u32 resource_id = (key >> 16) & 0xFFFF;
		struct regulator *reg;
		int uv;

		if (resource_id == CDSP_RESOURCE_ID_CX) {
			reg = drv->vdd_cx;
		} else if (resource_id == CDSP_RESOURCE_ID_MX) {
			if (!drv->vdd_mx) {
				dev_dbg(drv->dev,
					"KVP[%d]: MX rail not available on this board\n",
					i);
				ret = -EINVAL;
				goto send_response;
			}
			reg = drv->vdd_mx;
		} else {
			dev_err(drv->dev, "KVP[%d]: unknown key 0x%x\n", i, key);
			ret = -EINVAL;
			goto send_response;
		}

		/* Set voltage; the regulator framework enforces DTS constraints */
		ret = regulator_set_voltage(reg, voltage_uv, voltage_uv);
		if (ret) {
			dev_err(drv->dev, "KVP[%d]: failed to set %s voltage %u uV: %d\n",
				i, resource_id == CDSP_RESOURCE_ID_CX ? "CX" : "MX",
				voltage_uv, ret);
			goto send_response;
		}

		/* Read back actual voltage */
		uv = regulator_get_voltage(reg);
		if (uv < 0) {
			dev_warn(drv->dev, "KVP[%d]: failed to read back voltage: %d\n",
				 i, uv);
			uv = voltage_uv;
		}
		/* Track the last successfully set voltage to report in the response */
		actual_uv = uv;

		dev_dbg(drv->dev, "DCVS: Set %s to %d uV (requested %u uV)\n",
			resource_id == CDSP_RESOURCE_ID_CX ? "CX" : "MX",
			actual_uv, voltage_uv);
	}

send_response:
	/* Write response to SMEM response area */
	smem->response.msg_size     = CDSP_RESP_MSG_SIZE;
	smem->response.sequence     = sequence;
	smem->response.msg_id       = CDSP_MSG_ID_RESPONSE;
	smem->response.status       = ret;
	smem->response.data         = (ret == 0) ? actual_uv : 0;
	smem->response.timestamp_us = ktime_to_us(ktime_get());

	/*
	 * Update header statistics and clear in-flight flag.
	 * Order: write response data -> clear request_in_flight -> wmb ->
	 * send interrupt. NSP Q6 checks request_in_flight == 0 to know
	 * the response is ready.
	 */
	smem->hdr.apss_rx_count++;
	smem->hdr.apss_tx_count++;
	/* Mark channel idle before sending the interrupt */
	WRITE_ONCE(smem->hdr.request_in_flight, 0);
	/* Ensure response data is visible before sending interrupt to NSP Q6 */
	wmb();

	/* Send IPCC PING interrupt to NSP Q6 */
	mbox_send_message(drv->dcvs_mbox_chan, NULL);
	mbox_client_txdone(drv->dcvs_mbox_chan, 0);

	mutex_unlock(&drv->lock);
}

/**
 * cdsp_execute_isolation_sequence() - Execute isolation and power-down sequence
 * @drv: Driver context
 *
 * Executes the isolation sequence for power collapse as defined in
 * the MPM power collapse specification, followed by regulator disable.
 * Processes NSP/CX rail first (always), then MXC/MX rail (FULL_PC mode only).
 */
static void cdsp_execute_isolation_sequence(struct cdsp_power_driver *drv)
{
	/*
	 * When vdd_mx is absent (board has no MX regulator handle), always
	 * execute FULL_PC isolation for both CX and MX rails regardless of
	 * what the Q6 requested.
	 */
	int num_rails = (!drv->vdd_mx || cdsp_is_full_pc_mode(drv)) ? 2 : 1;
	unsigned int iso_regs[] = {
		VDD_RAIL_ISO_CTRL(VDD_RAIL_CX),  /* NSP/CX rail - always processed */
		VDD_RAIL_ISO_CTRL(VDD_RAIL_MX),  /* MXC/MX rail - FULL_PC only */
	};
	struct regulator *regulators[] = { drv->vdd_cx, drv->vdd_mx };
	struct regmap *mpm_map = drv->mpm_regmap;
	int ret, i;
	u32 val;

	for (i = 0; i < num_rails; i++) {
		unsigned int iso = iso_regs[i];

		/* Step 1: Disable clocks */
		regmap_update_bits(mpm_map, iso, ISO_CLK_DIS, ISO_CLK_DIS);

		/* Poll CLK_DIS_ACK only for NSP/CX rail (MXC CLK_DIS_ACK is tied to 0) */
		if (i == 0) {
			ret = regmap_read_poll_timeout(mpm_map, iso, val,
						       (val & ISO_CLK_DIS_ACK),
						       MPM_POLL_SLEEP_US, MPM_POLL_TIMEOUT_US);
			if (ret)
				dev_err(drv->dev, "Timeout waiting for NSP CLK_DIS_ACK\n");
		}

		/* Step 2: Isolate inputs */
		regmap_update_bits(mpm_map, iso,
				   ISO_INPUT | ISO_INPUT_CLKS | ISO_INPUT_DFT,
				   ISO_INPUT | ISO_INPUT_CLKS | ISO_INPUT_DFT);
		/* Step 3: Clamp memories */
		regmap_update_bits(mpm_map, iso, ISO_CLAMP_MEM, ISO_CLAMP_MEM);
		/* Step 4: Enable 0-pin retention */
		regmap_update_bits(mpm_map, iso, ISO_RET_0PIN, ISO_RET_0PIN);
		/* Step 5: Save to balloon latch (low-high-low pulse) */
		regmap_update_bits(mpm_map, iso, ISO_SAVE_FF, 0x0);
		regmap_update_bits(mpm_map, iso, ISO_SAVE_FF, ISO_SAVE_FF);
		regmap_update_bits(mpm_map, iso, ISO_SAVE_FF, 0x0);
		/* Step 6: Prepare restore signal (low-high) */
		regmap_update_bits(mpm_map, iso, ISO_RESTORE_FF, ISO_RESTORE_FF);
		/* Step 7: Freeze outputs */
		regmap_update_bits(mpm_map, iso, ISO_FREEZE_OUTPUT, ISO_FREEZE_OUTPUT);

		/* Step 8: Turn off regulator */
		if (regulators[i]) {
			ret = regulator_disable(regulators[i]);
			if (ret)
				dev_err(drv->dev, "Failed to disable %s rail: %d\n",
					i == 0 ? "CX" : "MX", ret);
			else
				usleep_range(8000, 10000);
		}
	}

	dev_dbg(drv->dev, "Isolation sequence complete (%s mode)\n",
		num_rails > 1 ? "FULL_PC" : "LONG_APCR");
}

/**
 * cdsp_execute_restoration_sequence() - Execute 9-step restoration sequence
 * @drv: Driver context
 *
 * Executes the restoration sequence for power restore as defined in
 * the MPM power collapse specification.
 * FULL_PC: Restores MXC (MX) rail first, then NSP (CX) rail
 * LONG_APCR: Restores NSP (CX) rail only
 */
static void cdsp_execute_restoration_sequence(struct cdsp_power_driver *drv)
{
	/*
	 * When vdd_mx is absent, always restore both CX and MX rails
	 * (FULL_PC), but skip regulator_enable for MX (no handle).
	 */
	int start_rail = (!drv->vdd_mx || cdsp_is_full_pc_mode(drv)) ? 0 : 1;
	unsigned int iso_regs[] = {
		VDD_RAIL_ISO_CTRL(VDD_RAIL_MX),  /* MXC/MX rail - FULL_PC only, restored first */
		VDD_RAIL_ISO_CTRL(VDD_RAIL_CX),  /* NSP/CX rail - always restored */
	};
	struct regulator *regulators[] = { drv->vdd_mx, drv->vdd_cx };
	struct regmap *mpm_map = drv->mpm_regmap;
	static const char * const rail_names[] = { "MX", "CX" };
	int ret, i;

	for (i = start_rail; i < ARRAY_SIZE(iso_regs); i++) {
		unsigned int iso = iso_regs[i];

		/* Step 1: Assert power-up reset */
		regmap_update_bits(mpm_map, iso, ISO_PWR_UP_RESET, ISO_PWR_UP_RESET);

		/* Step 2: Enable power rail while reset is asserted (skip if no handle) */
		if (regulators[i]) {
			ret = regulator_enable(regulators[i]);
			if (ret)
				dev_err(drv->dev, "Failed to enable %s: %d\n", rail_names[i], ret);
			else
				usleep_range(8000, 10000);
		}

		/* Step 3: Clear power-up reset */
		regmap_update_bits(mpm_map, iso, ISO_PWR_UP_RESET, 0);
		/* Step 4: Restore from balloon latch */
		regmap_update_bits(mpm_map, iso, ISO_RESTORE_FF, 0);
		/* Step 5: Remove memory clamp */
		regmap_update_bits(mpm_map, iso, ISO_CLAMP_MEM, 0);
		/* Step 6: Restore from 0-pin retention */
		regmap_update_bits(mpm_map, iso, ISO_RET_0PIN, 0);
		/* Step 7: Remove output freeze */
		regmap_update_bits(mpm_map, iso, ISO_FREEZE_OUTPUT, 0);
		/* Step 8: Remove input isolation */
		regmap_update_bits(mpm_map, iso, ISO_INPUT | ISO_INPUT_DFT | ISO_INPUT_CLKS, 0);
		/* Step 9: Enable clocks */
		regmap_update_bits(mpm_map, iso, ISO_CLK_DIS, 0);
	}

	dev_dbg(drv->dev, "Restoration sequence complete (%s mode)\n",
		start_rail == 0 ? "FULL_PC" : "LONG_APCR");
}

/**
 * cdsp_lpm_shutdown_sequence() - Execute LPM shutdown sequence
 * @drv: Driver context
 *
 * Handles power collapse request from CDSP:
 * 1. Completes MPM 4-phase handshake
 * 2. Executes isolation sequence
 * 3. Disables regulators
 *
 * The regulator_disable() calls here are balanced against the
 * regulator_enable() calls made by PAS remoteproc via the virtual
 * vdd-cx/vdd-mx regulators (which pass through to vdd_cx/vdd_mx).
 */
static void cdsp_lpm_shutdown_sequence(struct cdsp_power_driver *drv)
{
	struct regmap *mpm_map = drv->mpm_regmap;
	bool is_full_pc = cdsp_is_full_pc_mode(drv);
	u32 hdshk_status;
	int ret;

	dev_dbg(drv->dev, "LPM: Starting power collapse sequence\n");

	/* Phase 1: Acknowledge shutdown request */
	regmap_update_bits(mpm_map, CLIENT_RSC_HDSHK(0), MPM_SHUTDOWN_ACK, MPM_SHUTDOWN_ACK);

	/* Phase 2: Wait for request de-assertion */
	ret = regmap_read_poll_timeout(mpm_map, CLIENT_RSC_HDSHK(0), hdshk_status,
				       !(hdshk_status & MPM_SHUTDOWN_REQ),
				       MPM_POLL_SLEEP_US, MPM_POLL_TIMEOUT_US);
	if (ret) {
		dev_err(drv->dev, "Timeout waiting for shutdown_req clear\n");
		return;
	}

	/* Phase 3: Clear acknowledgment */
	regmap_update_bits(mpm_map, CLIENT_RSC_HDSHK(0), MPM_SHUTDOWN_ACK, 0x0);

	/* Phase 4: Execute isolation sequence */
	cdsp_execute_isolation_sequence(drv);

	atomic_set(&drv->power_state, CDSP_POWER_OFF);

	dev_dbg(drv->dev, "LPM: Power collapse complete (%s mode)\n",
		is_full_pc ? "FULL_PC" : "LONG_APCR");
}

/**
 * cdsp_lpm_restore_sequence() - Execute LPM restore sequence
 * @drv: Driver context
 *
 * Handles power restore request from CDSP:
 * 1. Executes restoration sequence (includes regulator enable)
 * 2. Completes MPM bringup handshake
 */
static void cdsp_lpm_restore_sequence(struct cdsp_power_driver *drv)
{
	struct regmap *mpm_map = drv->mpm_regmap;
	u32 hdshk_status;
	int ret;

	dev_dbg(drv->dev, "LPM: Starting power restore sequence\n");

	/* Execute restoration sequence (includes regulator enable) */
	cdsp_execute_restoration_sequence(drv);

	/* Assert BRINGUP_ACK */
	regmap_update_bits(mpm_map, CLIENT_RSC_HDSHK(0), MPM_BRINGUP_ACK, MPM_BRINGUP_ACK);

	/* Wait for BRINGUP_REQ to de-assert */
	ret = regmap_read_poll_timeout(mpm_map, CLIENT_RSC_HDSHK(0), hdshk_status,
				       !(hdshk_status & MPM_BRINGUP_REQ),
				       MPM_POLL_SLEEP_US, MPM_POLL_TIMEOUT_US);
	if (ret) {
		dev_err(drv->dev, "Timeout waiting for BRINGUP_REQ clear\n");
		return;
	}

	/* Clear BRINGUP_ACK */
	regmap_update_bits(mpm_map, CLIENT_RSC_HDSHK(0), MPM_BRINGUP_ACK, 0);

	atomic_set(&drv->power_state, CDSP_POWER_ON);

	dev_dbg(drv->dev, "LPM: Power restore complete\n");
}

/**
 * cdsp_lpm_irq_handler() - LPM interrupt handler
 * @irq: Interrupt number
 * @data: Driver context
 *
 * Called when CDSP sends an LPM request via MPM.
 * Schedules work to process the request.
 *
 * Return: IRQ_HANDLED
 */
static irqreturn_t cdsp_lpm_irq_handler(int irq, void *data)
{
	struct cdsp_power_driver *drv = data;
	unsigned int client_rsc_hdsk_irq_val;
	unsigned int rsc_hdsk_irq_val;

	regmap_read(drv->mpm_regmap, RSC_HDSHK_IRQ_STAT, &rsc_hdsk_irq_val);
	regmap_read(drv->mpm_regmap, CLIENT_RSC_IRQ_STAT(0), &client_rsc_hdsk_irq_val);
	if (!client_rsc_hdsk_irq_val || !rsc_hdsk_irq_val)
		return IRQ_NONE;

	/* Schedule work to process LPM request */
	queue_work(drv->lpm_wq, &drv->lpm_work);

	return IRQ_HANDLED;
}

/**
 * cdsp_lpm_work_fn() - LPM work function
 * @work: Work structure
 *
 * Processes LPM requests from CDSP by reading MPM handshake status
 * and executing appropriate shutdown or restore sequence.
 */
static void cdsp_lpm_work_fn(struct work_struct *work)
{
	struct cdsp_power_driver *drv = container_of(work,
						      struct cdsp_power_driver,
						      lpm_work);
	u32 hdshk_status;
	bool is_shutdown;

	mutex_lock(&drv->lock);

	/* Read MPM handshake status */
	regmap_read(drv->mpm_regmap, CLIENT_RSC_HDSHK(0), &hdshk_status);

	/* Clear the interrupt */
	regmap_write(drv->mpm_regmap, CLIENT_RSC_IRQ_CLR(0), 0x0);
	regmap_write(drv->mpm_regmap, CLIENT_RSC_IRQ_CLR(0), 0x1);
	regmap_write(drv->mpm_regmap, CLIENT_RSC_IRQ_CLR(0), 0x0);

	/* Determine if this is shutdown or bringup */
	is_shutdown = !!(hdshk_status & MPM_SHUTDOWN_REQ);

	if (is_shutdown) {
		if (atomic_read(&drv->power_state) == CDSP_POWER_OFF) {
			dev_warn(drv->dev, "Spurious shutdown request, already powered off\n");
			goto out_unlock;
		}
		cdsp_lpm_shutdown_sequence(drv);
	} else {
		if (atomic_read(&drv->power_state) == CDSP_POWER_ON) {
			dev_warn(drv->dev, "Spurious bringup request, already powered on\n");
			goto out_unlock;
		}
		cdsp_lpm_restore_sequence(drv);
	}

out_unlock:
	mutex_unlock(&drv->lock);
}

static const struct regmap_config cdsp_rscc_regmap_config = {
	.reg_bits       = 32,
	.reg_stride     = 4,
	.val_bits       = 32,
	.max_register   = 0x4,
	.fast_io        = true,
};

static const struct regmap_config cdsp_regmap_config = {
	.reg_bits       = 32,
	.reg_stride     = 4,
	.val_bits       = 32,
	.max_register   = 0x1000,
	.fast_io        = true,
};

/**
 * cdsp_power_probe() - Probe the CDSP power management driver
 * @pdev: Platform device
 *
 * Acquires the PMIC regulator consumer handles, registers the virtual
 * cdsp-vdd-cx (and optionally cdsp-vdd-mx) regulator providers, maps the
 * MPM and RSCC register regions, and registers the DCVS and LPM interrupt
 * handlers.
 *
 * Return: 0 on success, negative error code on failure
 */
static int cdsp_power_probe(struct platform_device *pdev)
{
	struct regulator_config virt_cfg = {};
	struct cdsp_power_driver *drv;
	struct regulator_dev *rdev;
	void __iomem *rscc_base;
	void __iomem *mpm_base;
	size_t smem_size;
	u32 smem_id;
	int ret;

	/* Allocate driver context */
	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->dev = &pdev->dev;
	mutex_init(&drv->lock);
	atomic_set(&drv->power_state, CDSP_POWER_ON);

	/* Get SMEM item ID from device tree */
	ret = of_property_read_u32(pdev->dev.of_node, "qcom,smem-item", &smem_id);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to get SMEM item ID\n");

	/* Create SMEM entry for DCVS */
	ret = qcom_smem_alloc(CDSP_SMEM_NSP_HOST_ID, smem_id, CDSP_SMEM_SIZE);
	if (ret && ret != -EEXIST)
		return dev_err_probe(&pdev->dev, ret, "Failed to allocate SMEM\n");

	/* Get SMEM pointer and validate size */
	drv->smem = qcom_smem_get(CDSP_SMEM_NSP_HOST_ID, smem_id, &smem_size);
	if (IS_ERR(drv->smem))
		return dev_err_probe(&pdev->dev, PTR_ERR(drv->smem),
				     "Failed to get SMEM\n");

	if (smem_size < CDSP_SMEM_SIZE)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "SMEM region too small: got %zu, expected %u\n",
				     smem_size, CDSP_SMEM_SIZE);

	/*
	 * Initialise the SMEM channel header.
	 * Zero the entire region first so all padding and reserved fields
	 * are clean, then fill in the fixed protocol fields.
	 * apss_state is set to 1 last (after wmb) so NSP Q6 only sees a
	 * fully-populated header once APSS is ready.
	 */
	memset(drv->smem, 0, sizeof(*drv->smem));
	drv->smem->hdr.magic           = CDSP_SMEM_MAGIC;
	drv->smem->hdr.version         = CDSP_SMEM_VERSION;
	drv->smem->hdr.request_offset  = CDSP_SMEM_REQUEST_OFFSET;
	drv->smem->hdr.request_size    = CDSP_SMEM_REQUEST_SIZE;
	drv->smem->hdr.response_offset = CDSP_SMEM_RESPONSE_OFFSET;
	drv->smem->hdr.response_size   = CDSP_SMEM_RESPONSE_SIZE;
	/* Signal APSS readiness to NSP Q6 */
	WRITE_ONCE(drv->smem->hdr.apss_state, 1);
	/* Ensure SMEM header is fully written before NSP Q6 reads it */
	wmb();

	/*
	 * Get voltage regulator consumer handles.
	 * These are the actual NSP_CX and NSP_MX voltage rails.
	 * The virtual regulator ops pass through to these handles.
	 */
	drv->vdd_cx = devm_regulator_get(&pdev->dev, "vdd-cx");
	if (IS_ERR(drv->vdd_cx))
		return dev_err_probe(&pdev->dev, PTR_ERR(drv->vdd_cx),
				     "Failed to get vdd-cx regulator\n");

	drv->vdd_mx = devm_regulator_get_optional(&pdev->dev, "vdd-mx");
	if (IS_ERR(drv->vdd_mx)) {
		if (PTR_ERR(drv->vdd_mx) != -ENODEV)
			return dev_err_probe(&pdev->dev, PTR_ERR(drv->vdd_mx),
					     "Failed to get vdd-mx regulator\n");
		drv->vdd_mx = NULL;
		dev_dbg(&pdev->dev, "No vdd-mx regulator, MX rail absent on this board\n");
	}

	/*
	 * Register virtual regulator provider.
	 *
	 * Expose vdd-cx and vdd-mx virtual regulators so that PAS remoteproc
	 * can consume them via cx-supply / mx-supply DTS properties.
	 * The enable/disable ops pass through to vdd_cx / vdd_mx above,
	 * making CDSP the sole hardware power manager for the NSP subsystem.
	 */
	virt_cfg.dev         = &pdev->dev;
	virt_cfg.driver_data = drv;
	virt_cfg.of_node     = pdev->dev.of_node;

	INIT_WORK(&drv->dcvs_work, cdsp_dcvs_work_fn);
	INIT_WORK(&drv->lpm_work, cdsp_lpm_work_fn);

	drv->lpm_wq = alloc_ordered_workqueue("cdsp_lpm_wq", 0);
	if (!drv->lpm_wq) {
		mbox_free_channel(drv->dcvs_mbox_chan);
		return dev_err_probe(&pdev->dev,
				     -ENOMEM,
				     "failed to allocate cdsp lpm workqueue\n");
	}

	rdev = devm_regulator_register(&pdev->dev,
				       &cdsp_virt_reg_descs[CDSP_VIRT_NSP_CX],
				       &virt_cfg);
	if (IS_ERR(rdev))
		return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
				     "Failed to register cdsp-vdd-cx virtual regulator\n");

	if (drv->vdd_mx) {
		rdev = devm_regulator_register(&pdev->dev,
					       &cdsp_virt_reg_descs[CDSP_VIRT_NSP_MX],
					       &virt_cfg);
		if (IS_ERR(rdev))
			return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
					     "Failed to register cdsp-vdd-mx virtual regulator\n");
	}

	/* Register DCVS interrupt */
	drv->dcvs_irq = platform_get_irq_byname(pdev, "dcvs");
	if (drv->dcvs_irq < 0)
		return dev_err_probe(&pdev->dev, drv->dcvs_irq,
				     "Failed to get DCVS IRQ\n");

	ret = devm_request_threaded_irq(&pdev->dev, drv->dcvs_irq,
					NULL, cdsp_dcvs_irq_handler,
					IRQF_ONESHOT, "cdsp-dcvs", drv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to request DCVS IRQ\n");

	/* Setup MPM for LPM */
	mpm_base = devm_platform_ioremap_resource_byname(pdev, "mpm");
	if (IS_ERR(mpm_base))
		return dev_err_probe(&pdev->dev, PTR_ERR(mpm_base),
				     "Failed to map MPM registers\n");

	drv->mpm_regmap = devm_regmap_init_mmio(&pdev->dev, mpm_base, &cdsp_regmap_config);
	if (IS_ERR(drv->mpm_regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(drv->mpm_regmap),
				     "Failed to init MPM regmap\n");

	/* Setup RSCC for power mode detection */
	rscc_base = devm_platform_ioremap_resource_byname(pdev, "rscc");
	if (IS_ERR(rscc_base))
		return dev_err_probe(&pdev->dev, PTR_ERR(rscc_base),
				     "Failed to map RSCC registers\n");

	drv->rscc_regmap = devm_regmap_init_mmio(&pdev->dev, rscc_base, &cdsp_rscc_regmap_config);
	if (IS_ERR(drv->rscc_regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(drv->rscc_regmap),
				     "Failed to init RSCC regmap\n");

	drv->lpm_irq = platform_get_irq_byname(pdev, "lpm");
	if (drv->lpm_irq < 0)
		return dev_err_probe(&pdev->dev, drv->lpm_irq,
				     "Failed to get LPM IRQ\n");

	ret = devm_request_threaded_irq(&pdev->dev, drv->lpm_irq,
					NULL, cdsp_lpm_irq_handler,
					IRQF_ONESHOT, "cdsp-lpm", drv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to request LPM IRQ\n");

	/* Setup mbox for DCVS response */
	drv->dcvs_mbox_client.dev = &pdev->dev;
	drv->dcvs_mbox_client.knows_txdone = true;
	drv->dcvs_mbox_chan = mbox_request_channel(&drv->dcvs_mbox_client, 0);
	if (IS_ERR(drv->dcvs_mbox_chan))
		return dev_err_probe(&pdev->dev, PTR_ERR(drv->dcvs_mbox_chan),
				     "Failed to get dcvs mbox channel\n");

	platform_set_drvdata(pdev, drv);

	dev_dbg(&pdev->dev, "CDSP power driver initialized\n");

	return 0;
}

static void cdsp_power_remove(struct platform_device *pdev)
{
	struct cdsp_power_driver *drv = platform_get_drvdata(pdev);

	/* Cancel any pending work */
	cancel_work_sync(&drv->dcvs_work);
	cancel_work_sync(&drv->lpm_work);

	if (drv->lpm_wq)
		destroy_workqueue(drv->lpm_wq);

	mbox_free_channel(drv->dcvs_mbox_chan);
}

static const struct of_device_id cdsp_power_of_match[] = {
	{ .compatible = "qcom,cdsp-power" },
	{ }
};
MODULE_DEVICE_TABLE(of, cdsp_power_of_match);

static struct platform_driver cdsp_power_driver = {
	.probe = cdsp_power_probe,
	.remove = cdsp_power_remove,
	.driver = {
		.name = "cdsp-power",
		.of_match_table = cdsp_power_of_match,
	},
};

module_platform_driver(cdsp_power_driver);

MODULE_DESCRIPTION("CDSP Power Management Driver");
MODULE_LICENSE("GPL");
