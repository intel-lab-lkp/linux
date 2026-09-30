// SPDX-License-Identifier: GPL-2.0
/*
 * Texas Instruments System Control Interface Protocol
 *
 * Copyright (C) 2015-2016 Texas Instruments Incorporated - https://www.ti.com/
 *	Nishanth Menon
 */

#ifndef __TISCI_PROTOCOL_H
#define __TISCI_PROTOCOL_H

#include <linux/device.h>
#include <linux/err.h>
#include <linux/types.h>
#include <linux/spinlock.h>

/**
 * struct ti_sci_version_info - version information structure
 * @abi_major:	Major ABI version. Change here implies risk of backward
 *		compatibility break.
 * @abi_minor:	Minor ABI version. Change here implies new feature addition,
 *		or compatible change in ABI.
 * @firmware_revision:	Firmware revision (not usually used).
 * @firmware_description: Firmware description (not usually used).
 */
struct ti_sci_version_info {
	u8 abi_major;
	u8 abi_minor;
	u16 firmware_revision;
	char firmware_description[32];
};

struct ti_sci_handle;

/**
 * struct ti_sci_dev_ops - Device control operations
 * @get_device: Command to request for device managed by TISCI
 *		Returns 0 for successful exclusive request, else returns
 *		corresponding error message.
 * @get_device_exclusive: Command to request for device managed by TISCI that is
 *		exclusively owned by the requesting host.
 *		Returns 0 for successful exclusive request, else returns
 *		corresponding error message.
 * @idle_device: Command to idle a device managed by TISCI
 *		Returns 0 for successful exclusive request, else returns
 *		corresponding error message.
 * @idle_device_exclusive: Command to idle a device managed by TISCI that is
 *		exclusively owned by requesting host.
 *		Returns 0 for successful exclusive request, else returns
 *		corresponding error message.
 * @put_device:	Command to release a device managed by TISCI
 *		Returns 0 for successful release, else returns corresponding
 *		error message.
 * @is_valid:	Check if the device ID is a valid ID.
 *		Returns 0 if the ID is valid, else returns corresponding error.
 * @get_context_loss_count: Command to retrieve context loss counter - this
 *		increments every time the device looses context. Overflow
 *		is possible.
 *		- count: pointer to u32 which will retrieve counter
 *		Returns 0 for successful information request and count has
 *		proper data, else returns corresponding error message.
 * @is_idle:	Reports back about device idle state
 *		- req_state: Returns requested idle state
 *		Returns 0 for successful information request and req_state and
 *		current_state has proper data, else returns corresponding error
 *		message.
 * @is_stop:	Reports back about device stop state
 *		- req_state: Returns requested stop state
 *		- current_state: Returns current stop state
 *		Returns 0 for successful information request and req_state and
 *		current_state has proper data, else returns corresponding error
 *		message.
 * @is_on:	Reports back about device ON(or active) state
 *		- req_state: Returns requested ON state
 *		- current_state: Returns current ON state
 *		Returns 0 for successful information request and req_state and
 *		current_state has proper data, else returns corresponding error
 *		message.
 * @is_transitioning: Reports back if the device is in the middle of transition
 *		of state.
 *		-current_state: Returns 'true' if currently transitioning.
 * @set_device_resets: Command to configure resets for device managed by TISCI.
 *		-reset_state: Device specific reset bit field
 *		Returns 0 for successful request, else returns
 *		corresponding error message.
 * @get_device_resets: Command to read state of resets for device managed
 *		by TISCI.
 *		-reset_state: pointer to u32 which will retrieve resets
 *		Returns 0 for successful request, else returns
 *		corresponding error message.
 *
 * NOTE: for all these functions, the following parameters are generic in
 * nature:
 * -handle:	Pointer to TISCI handle as retrieved by *ti_sci_get_handle
 * -id:		Device Identifier
 *
 * Request for the device - NOTE: the client MUST maintain integrity of
 * usage count by balancing get_device with put_device. No refcounting is
 * managed by driver for that purpose.
 */
struct ti_sci_dev_ops {
	int (*get_device)(const struct ti_sci_handle *handle, u32 id);
	int (*get_device_exclusive)(const struct ti_sci_handle *handle, u32 id);
	int (*idle_device)(const struct ti_sci_handle *handle, u32 id);
	int (*idle_device_exclusive)(const struct ti_sci_handle *handle,
				     u32 id);
	int (*put_device)(const struct ti_sci_handle *handle, u32 id);
	int (*is_valid)(const struct ti_sci_handle *handle, u32 id);
	int (*get_context_loss_count)(const struct ti_sci_handle *handle,
				      u32 id, u32 *count);
	int (*is_idle)(const struct ti_sci_handle *handle, u32 id,
		       bool *requested_state);
	int (*is_stop)(const struct ti_sci_handle *handle, u32 id,
		       bool *req_state, bool *current_state);
	int (*is_on)(const struct ti_sci_handle *handle, u32 id,
		     bool *req_state, bool *current_state);
	int (*is_transitioning)(const struct ti_sci_handle *handle, u32 id,
				bool *current_state);
	int (*set_device_resets)(const struct ti_sci_handle *handle, u32 id,
				 u32 reset_state);
	int (*get_device_resets)(const struct ti_sci_handle *handle, u32 id,
				 u32 *reset_state);
};

/**
 * struct ti_sci_clk_ops - Clock control operations
 * @get_clock:	Request for activation of clock and manage by processor
 *		- needs_ssc: 'true' if Spread Spectrum clock is desired.
 *		- can_change_freq: 'true' if frequency change is desired.
 *		- enable_input_term: 'true' if input termination is desired.
 * @idle_clock:	Request for Idling a clock managed by processor
 * @put_clock:	Release the clock to be auto managed by TISCI
 * @is_auto:	Is the clock being auto managed
 *		- req_state: state indicating if the clock is auto managed
 * @is_on:	Is the clock ON
 *		- req_state: if the clock is requested to be forced ON
 *		- current_state: if the clock is currently ON
 * @is_off:	Is the clock OFF
 *		- req_state: if the clock is requested to be forced OFF
 *		- current_state: if the clock is currently Gated
 * @set_parent:	Set the clock source of a specific device clock
 *		- parent_id: Parent clock identifier to set.
 * @get_parent:	Get the current clock source of a specific device clock
 *		- parent_id: Parent clock identifier which is the parent.
 * @get_num_parents: Get the number of parents of the current clock source
 *		- num_parents: returns the number of parent clocks.
 * @get_best_match_freq: Find a best matching frequency for a frequency
 *		range.
 *		- match_freq: Best matching frequency in Hz.
 * @set_freq:	Set the Clock frequency
 * @get_freq:	Get the Clock frequency
 *		- current_freq: Frequency in Hz that the clock is at.
 *
 * NOTE: for all these functions, the following parameters are generic in
 * nature:
 * -handle:	Pointer to TISCI handle as retrieved by *ti_sci_get_handle
 * -did:	Device identifier this request is for
 * -cid:	Clock identifier for the device for this request.
 *		Each device has it's own set of clock inputs. This indexes
 *		which clock input to modify.
 * -min_freq:	The minimum allowable frequency in Hz. This is the minimum
 *		allowable programmed frequency and does not account for clock
 *		tolerances and jitter.
 * -target_freq: The target clock frequency in Hz. A frequency will be
 *		processed as close to this target frequency as possible.
 * -max_freq:	The maximum allowable frequency in Hz. This is the maximum
 *		allowable programmed frequency and does not account for clock
 *		tolerances and jitter.
 *
 * Request for the clock - NOTE: the client MUST maintain integrity of
 * usage count by balancing get_clock with put_clock. No refcounting is
 * managed by driver for that purpose.
 */
struct ti_sci_clk_ops {
	int (*get_clock)(const struct ti_sci_handle *handle, u32 did, u32 cid,
			 bool needs_ssc, bool can_change_freq,
			 bool enable_input_term);
	int (*idle_clock)(const struct ti_sci_handle *handle, u32 did, u32 cid);
	int (*put_clock)(const struct ti_sci_handle *handle, u32 did, u32 cid);
	int (*is_auto)(const struct ti_sci_handle *handle, u32 did, u32 cid,
		       bool *req_state);
	int (*is_on)(const struct ti_sci_handle *handle, u32 did, u32 cid,
		     bool *req_state, bool *current_state);
	int (*is_off)(const struct ti_sci_handle *handle, u32 did, u32 cid,
		      bool *req_state, bool *current_state);
	int (*set_parent)(const struct ti_sci_handle *handle, u32 did, u32 cid,
			  u32 parent_id);
	int (*get_parent)(const struct ti_sci_handle *handle, u32 did, u32 cid,
			  u32 *parent_id);
	int (*get_num_parents)(const struct ti_sci_handle *handle, u32 did,
			       u32 cid, u32 *num_parents);
	int (*get_best_match_freq)(const struct ti_sci_handle *handle, u32 did,
				   u32 cid, u64 min_freq, u64 target_freq,
				   u64 max_freq, u64 *match_freq);
	int (*set_freq)(const struct ti_sci_handle *handle, u32 did, u32 cid,
			u64 min_freq, u64 target_freq, u64 max_freq);
	int (*get_freq)(const struct ti_sci_handle *handle, u32 did, u32 cid,
			u64 *current_freq);
};

/* TISCI LPM IO isolation control values */
#define TISCI_MSG_VALUE_IO_ENABLE			1
#define TISCI_MSG_VALUE_IO_DISABLE			0

/* TISCI LPM constraint state values */
#define TISCI_MSG_CONSTRAINT_SET			1
#define TISCI_MSG_CONSTRAINT_CLR			0

/**
 * struct ti_sci_pm_ops - Low Power Mode (LPM) control operations
 * @lpm_wake_reason: Get the wake up source that woke the SoC from LPM
 *		- source: The wake up source that woke soc from LPM.
 *		- timestamp: Timestamp at which soc woke.
 * @set_device_constraint: Set LPM constraint on behalf of a device
 *		- id: Device Identifier
 *		- state: The desired state of device constraint: set or clear.
 * @set_latency_constraint: Set LPM resume latency constraint
 *		- latency: maximum acceptable latency to wake up from low power mode
 *		- state: The desired state of latency constraint: set or clear.
 */
struct ti_sci_pm_ops {
	int (*lpm_wake_reason)(const struct ti_sci_handle *handle,
			       u32 *source, u64 *timestamp, u8 *pin, u8 *mode);
	int (*set_device_constraint)(const struct ti_sci_handle *handle,
				     u32 id, u8 state);
	int (*set_latency_constraint)(const struct ti_sci_handle *handle,
				      u16 latency, u8 state);
};

/**
 * struct ti_sci_resource_desc - Description of TI SCI resource instance range.
 * @start:	Start index of the first resource range.
 * @num:	Number of resources in the first range.
 * @start_sec:	Start index of the second resource range.
 * @num_sec:	Number of resources in the second range.
 * @res_map:	Bitmap to manage the allocation of these resources.
 */
struct ti_sci_resource_desc {
	u16 start;
	u16 num;
	u16 start_sec;
	u16 num_sec;
	unsigned long *res_map;
};

/**
 * struct ti_sci_rm_core_ops - Resource management core operations
 * @get_range:		Get a range of resources belonging to ti sci host.
 * @get_range_from_shost:	Get a range of resources belonging to
 *				specified host id.
 *			- s_host: Host processing entity to which the
 *				  resources are allocated
 *
 * NOTE: for these functions, all the parameters are consolidated and defined
 * as below:
 * - handle:	Pointer to TISCI handle as retrieved by *ti_sci_get_handle
 * - dev_id:	TISCI device ID.
 * - subtype:	Resource assignment subtype that is being requested
 *		from the given device.
 * - desc:	Pointer to ti_sci_resource_desc to be updated with the resource
 *		range start index and number of resources
 */
struct ti_sci_rm_core_ops {
	int (*get_range)(const struct ti_sci_handle *handle, u32 dev_id,
			 u8 subtype, struct ti_sci_resource_desc *desc);
	int (*get_range_from_shost)(const struct ti_sci_handle *handle,
				    u32 dev_id, u8 subtype, u8 s_host,
				    struct ti_sci_resource_desc *desc);
};

#define TI_SCI_RESASG_SUBTYPE_IR_OUTPUT		0
#define TI_SCI_RESASG_SUBTYPE_IA_VINT		0xa
#define TI_SCI_RESASG_SUBTYPE_GLOBAL_EVENT_SEVT	0xd
/**
 * struct ti_sci_rm_irq_ops: IRQ management operations
 * @set_irq:		Set an IRQ route between the requested source
 *			and destination
 * @set_event_map:	Set an Event based peripheral irq to Interrupt
 *			Aggregator.
 * @free_irq:		Free an IRQ route between the requested source
 *			and destination.
 * @free_event_map:	Free an event based peripheral irq to Interrupt
 *			Aggregator.
 */
struct ti_sci_rm_irq_ops {
	int (*set_irq)(const struct ti_sci_handle *handle, u16 src_id,
		       u16 src_index, u16 dst_id, u16 dst_host_irq);
	int (*set_event_map)(const struct ti_sci_handle *handle, u16 src_id,
			     u16 src_index, u16 ia_id, u16 vint,
			     u16 global_event, u8 vint_status_bit);
	int (*free_irq)(const struct ti_sci_handle *handle, u16 src_id,
			u16 src_index, u16 dst_id, u16 dst_host_irq);
	int (*free_event_map)(const struct ti_sci_handle *handle, u16 src_id,
			      u16 src_index, u16 ia_id, u16 vint,
			      u16 global_event, u8 vint_status_bit);
};

/* RA config.addr_lo parameter is valid for RM ring configure TI_SCI message */
#define TI_SCI_MSG_VALUE_RM_RING_ADDR_LO_VALID	BIT(0)
/* RA config.addr_hi parameter is valid for RM ring configure TI_SCI message */
#define TI_SCI_MSG_VALUE_RM_RING_ADDR_HI_VALID	BIT(1)
 /* RA config.count parameter is valid for RM ring configure TI_SCI message */
#define TI_SCI_MSG_VALUE_RM_RING_COUNT_VALID	BIT(2)
/* RA config.mode parameter is valid for RM ring configure TI_SCI message */
#define TI_SCI_MSG_VALUE_RM_RING_MODE_VALID	BIT(3)
/* RA config.size parameter is valid for RM ring configure TI_SCI message */
#define TI_SCI_MSG_VALUE_RM_RING_SIZE_VALID	BIT(4)
/* RA config.order_id parameter is valid for RM ring configure TISCI message */
#define TI_SCI_MSG_VALUE_RM_RING_ORDER_ID_VALID	BIT(5)
/* RA config.virtid parameter is valid for RM ring configure TISCI message */
#define TI_SCI_MSG_VALUE_RM_RING_VIRTID_VALID	BIT(6)
/* RA config.asel parameter is valid for RM ring configure TISCI message */
#define TI_SCI_MSG_VALUE_RM_RING_ASEL_VALID	BIT(7)

#define TI_SCI_MSG_VALUE_RM_ALL_NO_ORDER \
	(TI_SCI_MSG_VALUE_RM_RING_ADDR_LO_VALID | \
	TI_SCI_MSG_VALUE_RM_RING_ADDR_HI_VALID | \
	TI_SCI_MSG_VALUE_RM_RING_COUNT_VALID | \
	TI_SCI_MSG_VALUE_RM_RING_MODE_VALID | \
	TI_SCI_MSG_VALUE_RM_RING_SIZE_VALID | \
	TI_SCI_MSG_VALUE_RM_RING_ASEL_VALID)

/**
 * struct ti_sci_msg_rm_ring_cfg - Ring configuration
 *
 * Parameters for Navigator Subsystem ring configuration
 * See @ti_sci_msg_rm_ring_cfg_req
 *
 * @valid_params: Bitfield defining validity of ring configuration parameters.
 *	The ring configuration fields are not valid, and will not be used for
 *	ring configuration, if their corresponding valid bit is zero.
 *	Valid bit usage:
 *	0 - Valid bit for @addr_lo
 *	1 - Valid bit for @addr_hi
 *	2 - Valid bit for @count
 *	3 - Valid bit for @mode
 *	4 - Valid bit for @size
 *	5 - Valid bit for @order_id
 *	6 - Valid bit for @virtid
 *	7 - Valid bit for @asel
 * @nav_id: Device ID of Navigator Subsystem from which the ring is allocated
 * @index: ring index to be configured.
 * @addr_lo: 32 LSBs of ring base address to be programmed into the ring's
 *	RING_BA_LO register
 * @addr_hi: 16 MSBs of ring base address to be programmed into the ring's
 *	RING_BA_HI register.
 * @count: Number of ring elements. Must be even if mode is CREDENTIALS or QM
 *	modes.
 * @mode: Specifies the mode the ring is to be configured.
 * @size: Specifies encoded ring element size. To calculate the encoded size use
 *	the formula (log2(size_bytes) - 2), where size_bytes cannot be
 *	greater than 256.
 * @order_id: Specifies the ring's bus order ID.
 * @virtid: Ring virt ID value
 * @asel: Ring ASEL (address select) value to be set into the ASEL field of the
 *	ring's RING_BA_HI register.
 */
struct ti_sci_msg_rm_ring_cfg {
	u32 valid_params;
	u16 nav_id;
	u16 index;
	u32 addr_lo;
	u32 addr_hi;
	u32 count;
	u8 mode;
	u8 size;
	u8 order_id;
	u16 virtid;
	u8 asel;
};

/**
 * struct ti_sci_rm_ringacc_ops - Ring Accelerator Management operations
 * @set_cfg: configure the SoC Navigator Subsystem Ring Accelerator ring
 */
struct ti_sci_rm_ringacc_ops {
	int (*set_cfg)(const struct ti_sci_handle *handle,
		       const struct ti_sci_msg_rm_ring_cfg *params);
};

/**
 * struct ti_sci_rm_psil_ops - PSI-L thread operations
 * @pair: pair PSI-L source thread to a destination thread.
 *	If the src_thread is mapped to UDMA tchan, the corresponding channel's
 *	TCHAN_THRD_ID register is updated.
 *	If the dst_thread is mapped to UDMA rchan, the corresponding channel's
 *	RCHAN_THRD_ID register is updated.
 * @unpair: unpair PSI-L source thread from a destination thread.
 *	If the src_thread is mapped to UDMA tchan, the corresponding channel's
 *	TCHAN_THRD_ID register is cleared.
 *	If the dst_thread is mapped to UDMA rchan, the corresponding channel's
 *	RCHAN_THRD_ID register is cleared.
 */
struct ti_sci_rm_psil_ops {
	int (*pair)(const struct ti_sci_handle *handle, u32 nav_id,
		    u32 src_thread, u32 dst_thread);
	int (*unpair)(const struct ti_sci_handle *handle, u32 nav_id,
		      u32 src_thread, u32 dst_thread);
};

/* UDMAP channel types */
#define TI_SCI_RM_UDMAP_CHAN_TYPE_PKT_PBRR		2
#define TI_SCI_RM_UDMAP_CHAN_TYPE_PKT_PBRR_SB		3	/* RX only */
#define TI_SCI_RM_UDMAP_CHAN_TYPE_3RDP_PBRR		10
#define TI_SCI_RM_UDMAP_CHAN_TYPE_3RDP_PBVR		11
#define TI_SCI_RM_UDMAP_CHAN_TYPE_3RDP_BCOPY_PBRR	12
#define TI_SCI_RM_UDMAP_CHAN_TYPE_3RDP_BCOPY_PBVR	13

#define TI_SCI_RM_UDMAP_RX_FLOW_DESC_HOST		0
#define TI_SCI_RM_UDMAP_RX_FLOW_DESC_MONO		2

#define TI_SCI_RM_UDMAP_CHAN_BURST_SIZE_64_BYTES	1
#define TI_SCI_RM_UDMAP_CHAN_BURST_SIZE_128_BYTES	2
#define TI_SCI_RM_UDMAP_CHAN_BURST_SIZE_256_BYTES	3

#define TI_SCI_RM_BCDMA_EXTENDED_CH_TYPE_TCHAN		0
#define TI_SCI_RM_BCDMA_EXTENDED_CH_TYPE_BCHAN		1

/* UDMAP TX/RX channel valid_params common declarations */
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_PAUSE_ON_ERR_VALID		BIT(0)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_ATYPE_VALID                BIT(1)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_CHAN_TYPE_VALID            BIT(2)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_FETCH_SIZE_VALID           BIT(3)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_CQ_QNUM_VALID              BIT(4)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_PRIORITY_VALID             BIT(5)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_QOS_VALID                  BIT(6)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_ORDER_ID_VALID             BIT(7)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_SCHED_PRIORITY_VALID       BIT(8)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_BURST_SIZE_VALID		BIT(14)

/**
 * struct ti_sci_msg_rm_udmap_tx_ch_cfg - Configures a Navigator Subsystem UDMAP
 *					  transmit channel
 *
 * @valid_params: Bitfield defining validity of tx channel configuration
 * parameters. The tx channel configuration fields are not valid, and will not
 * be used for ch configuration, if their corresponding valid bit is zero.
 * Valid bit usage:
 *    0 - Valid bit for @tx_pause_on_err
 *    1 - Valid bit for @tx_atype
 *    2 - Valid bit for @tx_chan_type
 *    3 - Valid bit for @tx_fetch_size
 *    4 - Valid bit for @txcq_qnum
 *    5 - Valid bit for @tx_priority
 *    6 - Valid bit for @tx_qos
 *    7 - Valid bit for @tx_orderid
 *    8 - Valid bit for @tx_sched_priority
 *    9 - Valid bit for @tx_filt_einfo
 *   10 - Valid bit for @tx_filt_pswords
 *   11 - Valid bit for @tx_supr_tdpkt
 *   12 - Valid bit for @tx_credit_count
 *   13 - Valid bit for @fdepth
 *   14 - Valid bit for @tx_burst_size
 *   15 - Valid bit for @tx_tdtype
 *   16 - Valid bit for @extended_ch_type
 *
 * @nav_id: SoC device ID of Navigator Subsystem where tx channel is located
 *
 * @index: UDMAP transmit channel index.
 *
 * @tx_pause_on_err: UDMAP transmit channel pause on error configuration to
 * be programmed into the tx_pause_on_err field of the channel's TCHAN_TCFG
 * register.
 *
 * @tx_filt_einfo: UDMAP transmit channel extended packet information passing
 * configuration to be programmed into the tx_filt_einfo field of the
 * channel's TCHAN_TCFG register.
 *
 * @tx_filt_pswords: UDMAP transmit channel protocol specific word passing
 * configuration to be programmed into the tx_filt_pswords field of the
 * channel's TCHAN_TCFG register.
 *
 * @tx_atype: UDMAP transmit channel non Ring Accelerator access pointer
 * interpretation configuration to be programmed into the tx_atype field of
 * the channel's TCHAN_TCFG register.
 *
 * @tx_chan_type: UDMAP transmit channel functional channel type and work
 * passing mechanism configuration to be programmed into the tx_chan_type
 * field of the channel's TCHAN_TCFG register.
 *
 * @tx_supr_tdpkt: UDMAP transmit channel teardown packet generation suppression
 * configuration to be programmed into the tx_supr_tdpkt field of the channel's
 * TCHAN_TCFG register.
 *
 * @tx_fetch_size: UDMAP transmit channel number of 32-bit descriptor words to
 * fetch configuration to be programmed into the tx_fetch_size field of the
 * channel's TCHAN_TCFG register.  The user must make sure to set the maximum
 * word count that can pass through the channel for any allowed descriptor type.
 *
 * @tx_credit_count: UDMAP transmit channel transfer request credit count
 * configuration to be programmed into the count field of the TCHAN_TCREDIT
 * register.  Specifies how many credits for complete TRs are available.
 *
 * @txcq_qnum: UDMAP transmit channel completion queue configuration to be
 * programmed into the txcq_qnum field of the TCHAN_TCQ register. The specified
 * completion queue must be assigned to the host, or a subordinate of the host,
 * requesting configuration of the transmit channel.
 *
 * @tx_priority: UDMAP transmit channel transmit priority value to be programmed
 * into the priority field of the channel's TCHAN_TPRI_CTRL register.
 *
 * @tx_qos: UDMAP transmit channel transmit qos value to be programmed into the
 * qos field of the channel's TCHAN_TPRI_CTRL register.
 *
 * @tx_orderid: UDMAP transmit channel bus order id value to be programmed into
 * the orderid field of the channel's TCHAN_TPRI_CTRL register.
 *
 * @fdepth: UDMAP transmit channel FIFO depth configuration to be programmed
 * into the fdepth field of the TCHAN_TFIFO_DEPTH register. Sets the number of
 * Tx FIFO bytes which are allowed to be stored for the channel. Check the UDMAP
 * section of the TRM for restrictions regarding this parameter.
 *
 * @tx_sched_priority: UDMAP transmit channel tx scheduling priority
 * configuration to be programmed into the priority field of the channel's
 * TCHAN_TST_SCHED register.
 *
 * @tx_burst_size: UDMAP transmit channel burst size configuration to be
 * programmed into the tx_burst_size field of the TCHAN_TCFG register.
 *
 * @tx_tdtype: UDMAP transmit channel teardown type configuration to be
 * programmed into the tdtype field of the TCHAN_TCFG register:
 * 0 - Return immediately
 * 1 - Wait for completion message from remote peer
 *
 * @extended_ch_type: Valid for BCDMA.
 * 0 - the channel is split tx channel (tchan)
 * 1 - the channel is block copy channel (bchan)
 */
struct ti_sci_msg_rm_udmap_tx_ch_cfg {
	u32 valid_params;
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_TX_FILT_EINFO_VALID        BIT(9)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_TX_FILT_PSWORDS_VALID      BIT(10)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_TX_SUPR_TDPKT_VALID        BIT(11)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_TX_CREDIT_COUNT_VALID      BIT(12)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_TX_FDEPTH_VALID            BIT(13)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_TX_TDTYPE_VALID            BIT(15)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_EXTENDED_CH_TYPE_VALID	BIT(16)
	u16 nav_id;
	u16 index;
	u8 tx_pause_on_err;
	u8 tx_filt_einfo;
	u8 tx_filt_pswords;
	u8 tx_atype;
	u8 tx_chan_type;
	u8 tx_supr_tdpkt;
	u16 tx_fetch_size;
	u8 tx_credit_count;
	u16 txcq_qnum;
	u8 tx_priority;
	u8 tx_qos;
	u8 tx_orderid;
	u16 fdepth;
	u8 tx_sched_priority;
	u8 tx_burst_size;
	u8 tx_tdtype;
	u8 extended_ch_type;
};

/**
 * struct ti_sci_msg_rm_udmap_rx_ch_cfg - Configures a Navigator Subsystem UDMAP
 *					  receive channel
 *
 * @valid_params: Bitfield defining validity of rx channel configuration
 * parameters.
 * The rx channel configuration fields are not valid, and will not be used for
 * ch configuration, if their corresponding valid bit is zero.
 * Valid bit usage:
 *    0 - Valid bit for @rx_pause_on_err
 *    1 - Valid bit for @rx_atype
 *    2 - Valid bit for @rx_chan_type
 *    3 - Valid bit for @rx_fetch_size
 *    4 - Valid bit for @rxcq_qnum
 *    5 - Valid bit for @rx_priority
 *    6 - Valid bit for @rx_qos
 *    7 - Valid bit for @rx_orderid
 *    8 - Valid bit for @rx_sched_priority
 *    9 - Valid bit for @flowid_start
 *   10 - Valid bit for @flowid_cnt
 *   11 - Valid bit for @rx_ignore_short
 *   12 - Valid bit for @rx_ignore_long
 *   14 - Valid bit for @rx_burst_size
 *
 * @nav_id: SoC device ID of Navigator Subsystem where rx channel is located
 *
 * @index: UDMAP receive channel index.
 *
 * @rx_fetch_size: UDMAP receive channel number of 32-bit descriptor words to
 * fetch configuration to be programmed into the rx_fetch_size field of the
 * channel's RCHAN_RCFG register.
 *
 * @rxcq_qnum: UDMAP receive channel completion queue configuration to be
 * programmed into the rxcq_qnum field of the RCHAN_RCQ register.
 * The specified completion queue must be assigned to the host, or a subordinate
 * of the host, requesting configuration of the receive channel.
 *
 * @rx_priority: UDMAP receive channel receive priority value to be programmed
 * into the priority field of the channel's RCHAN_RPRI_CTRL register.
 *
 * @rx_qos: UDMAP receive channel receive qos value to be programmed into the
 * qos field of the channel's RCHAN_RPRI_CTRL register.
 *
 * @rx_orderid: UDMAP receive channel bus order id value to be programmed into
 * the orderid field of the channel's RCHAN_RPRI_CTRL register.
 *
 * @rx_sched_priority: UDMAP receive channel rx scheduling priority
 * configuration to be programmed into the priority field of the channel's
 * RCHAN_RST_SCHED register.
 *
 * @flowid_start: UDMAP receive channel additional flows starting index
 * configuration to program into the flow_start field of the RCHAN_RFLOW_RNG
 * register. Specifies the starting index for flow IDs the receive channel is to
 * make use of beyond the default flow. flowid_start and @flowid_cnt must be
 * set as valid and configured together. The starting flow ID set by
 * @flowid_start must be a flow index within the Navigator Subsystem's subset
 * of flows beyond the default flows statically mapped to receive channels.
 * The additional flows must be assigned to the host, or a subordinate of the
 * host, requesting configuration of the receive channel.
 *
 * @flowid_cnt: UDMAP receive channel additional flows count configuration to
 * program into the flowid_cnt field of the RCHAN_RFLOW_RNG register.
 * This field specifies how many flow IDs are in the additional contiguous range
 * of legal flow IDs for the channel.  @flowid_start and flowid_cnt must be
 * set as valid and configured together. Disabling the valid_params field bit
 * for flowid_cnt indicates no flow IDs other than the default are to be
 * allocated and used by the receive channel. @flowid_start plus flowid_cnt
 * cannot be greater than the number of receive flows in the receive channel's
 * Navigator Subsystem.  The additional flows must be assigned to the host, or a
 * subordinate of the host, requesting configuration of the receive channel.
 *
 * @rx_pause_on_err: UDMAP receive channel pause on error configuration to be
 * programmed into the rx_pause_on_err field of the channel's RCHAN_RCFG
 * register.
 *
 * @rx_atype: UDMAP receive channel non Ring Accelerator access pointer
 * interpretation configuration to be programmed into the rx_atype field of the
 * channel's RCHAN_RCFG register.
 *
 * @rx_chan_type: UDMAP receive channel functional channel type and work passing
 * mechanism configuration to be programmed into the rx_chan_type field of the
 * channel's RCHAN_RCFG register.
 *
 * @rx_ignore_short: UDMAP receive channel short packet treatment configuration
 * to be programmed into the rx_ignore_short field of the RCHAN_RCFG register.
 *
 * @rx_ignore_long: UDMAP receive channel long packet treatment configuration to
 * be programmed into the rx_ignore_long field of the RCHAN_RCFG register.
 *
 * @rx_burst_size: UDMAP receive channel burst size configuration to be
 * programmed into the rx_burst_size field of the RCHAN_RCFG register.
 */
struct ti_sci_msg_rm_udmap_rx_ch_cfg {
	u32 valid_params;
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_RX_FLOWID_START_VALID      BIT(9)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_RX_FLOWID_CNT_VALID        BIT(10)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_RX_IGNORE_SHORT_VALID      BIT(11)
#define TI_SCI_MSG_VALUE_RM_UDMAP_CH_RX_IGNORE_LONG_VALID       BIT(12)
	u16 nav_id;
	u16 index;
	u16 rx_fetch_size;
	u16 rxcq_qnum;
	u8 rx_priority;
	u8 rx_qos;
	u8 rx_orderid;
	u8 rx_sched_priority;
	u16 flowid_start;
	u16 flowid_cnt;
	u8 rx_pause_on_err;
	u8 rx_atype;
	u8 rx_chan_type;
	u8 rx_ignore_short;
	u8 rx_ignore_long;
	u8 rx_burst_size;
};

/**
 * struct ti_sci_msg_rm_udmap_flow_cfg - Configures a Navigator Subsystem UDMAP
 *					 receive flow
 *
 * @valid_params:
 * Bitfield defining validity of rx flow configuration parameters.  The
 * rx flow configuration fields are not valid, and will not be used for flow
 * configuration, if their corresponding valid bit is zero.  Valid bit usage:
 *     0 - Valid bit for @rx_einfo_present
 *     1 - Valid bit for @rx_psinfo_present
 *     2 - Valid bit for @rx_error_handling
 *     3 - Valid bit for @rx_desc_type
 *     4 - Valid bit for @rx_sop_offset
 *     5 - Valid bit for @rx_dest_qnum
 *     6 - Valid bit for @rx_src_tag_hi
 *     7 - Valid bit for @rx_src_tag_lo
 *     8 - Valid bit for @rx_dest_tag_hi
 *     9 - Valid bit for @rx_dest_tag_lo
 *    10 - Valid bit for @rx_src_tag_hi_sel
 *    11 - Valid bit for @rx_src_tag_lo_sel
 *    12 - Valid bit for @rx_dest_tag_hi_sel
 *    13 - Valid bit for @rx_dest_tag_lo_sel
 *    14 - Valid bit for @rx_fdq0_sz0_qnum
 *    15 - Valid bit for @rx_fdq1_qnum
 *    16 - Valid bit for @rx_fdq2_qnum
 *    17 - Valid bit for @rx_fdq3_qnum
 *    18 - Valid bit for @rx_ps_location
 *
 * @nav_id: SoC device ID of Navigator Subsystem from which the receive flow is
 * allocated
 *
 * @flow_index: UDMAP receive flow index for non-optional configuration.
 *
 * @rx_einfo_present:
 * UDMAP receive flow extended packet info present configuration to be
 * programmed into the rx_einfo_present field of the flow's RFLOW_RFA register.
 *
 * @rx_psinfo_present:
 * UDMAP receive flow PS words present configuration to be programmed into the
 * rx_psinfo_present field of the flow's RFLOW_RFA register.
 *
 * @rx_error_handling:
 * UDMAP receive flow error handling configuration to be programmed into the
 * rx_error_handling field of the flow's RFLOW_RFA register.
 *
 * @rx_desc_type:
 * UDMAP receive flow descriptor type configuration to be programmed into the
 * rx_desc_type field of the flow's RFLOW_RFA register.
 *
 * @rx_sop_offset:
 * UDMAP receive flow start of packet offset configuration to be programmed
 * into the rx_sop_offset field of the RFLOW_RFA register.  See the UDMAP
 * section of the TRM for more information on this setting.  Valid values for
 * this field are 0-255 bytes.
 *
 * @rx_dest_qnum:
 * UDMAP receive flow destination queue configuration to be programmed into the
 * rx_dest_qnum field of the flow's RFLOW_RFA register.  The specified
 * destination queue must be valid within the Navigator Subsystem and must be
 * owned by the host, or a subordinate of the host, requesting allocation and
 * configuration of the receive flow.
 *
 * @rx_src_tag_hi:
 * UDMAP receive flow source tag high byte constant configuration to be
 * programmed into the rx_src_tag_hi field of the flow's RFLOW_RFB register.
 * See the UDMAP section of the TRM for more information on this setting.
 *
 * @rx_src_tag_lo:
 * UDMAP receive flow source tag low byte constant configuration to be
 * programmed into the rx_src_tag_lo field of the flow's RFLOW_RFB register.
 * See the UDMAP section of the TRM for more information on this setting.
 *
 * @rx_dest_tag_hi:
 * UDMAP receive flow destination tag high byte constant configuration to be
 * programmed into the rx_dest_tag_hi field of the flow's RFLOW_RFB register.
 * See the UDMAP section of the TRM for more information on this setting.
 *
 * @rx_dest_tag_lo:
 * UDMAP receive flow destination tag low byte constant configuration to be
 * programmed into the rx_dest_tag_lo field of the flow's RFLOW_RFB register.
 * See the UDMAP section of the TRM for more information on this setting.
 *
 * @rx_src_tag_hi_sel:
 * UDMAP receive flow source tag high byte selector configuration to be
 * programmed into the rx_src_tag_hi_sel field of the RFLOW_RFC register.  See
 * the UDMAP section of the TRM for more information on this setting.
 *
 * @rx_src_tag_lo_sel:
 * UDMAP receive flow source tag low byte selector configuration to be
 * programmed into the rx_src_tag_lo_sel field of the RFLOW_RFC register.  See
 * the UDMAP section of the TRM for more information on this setting.
 *
 * @rx_dest_tag_hi_sel:
 * UDMAP receive flow destination tag high byte selector configuration to be
 * programmed into the rx_dest_tag_hi_sel field of the RFLOW_RFC register.  See
 * the UDMAP section of the TRM for more information on this setting.
 *
 * @rx_dest_tag_lo_sel:
 * UDMAP receive flow destination tag low byte selector configuration to be
 * programmed into the rx_dest_tag_lo_sel field of the RFLOW_RFC register.  See
 * the UDMAP section of the TRM for more information on this setting.
 *
 * @rx_fdq0_sz0_qnum:
 * UDMAP receive flow free descriptor queue 0 configuration to be programmed
 * into the rx_fdq0_sz0_qnum field of the flow's RFLOW_RFD register.  See the
 * UDMAP section of the TRM for more information on this setting. The specified
 * free queue must be valid within the Navigator Subsystem and must be owned
 * by the host, or a subordinate of the host, requesting allocation and
 * configuration of the receive flow.
 *
 * @rx_fdq1_qnum:
 * UDMAP receive flow free descriptor queue 1 configuration to be programmed
 * into the rx_fdq1_qnum field of the flow's RFLOW_RFD register.  See the
 * UDMAP section of the TRM for more information on this setting.  The specified
 * free queue must be valid within the Navigator Subsystem and must be owned
 * by the host, or a subordinate of the host, requesting allocation and
 * configuration of the receive flow.
 *
 * @rx_fdq2_qnum:
 * UDMAP receive flow free descriptor queue 2 configuration to be programmed
 * into the rx_fdq2_qnum field of the flow's RFLOW_RFE register.  See the
 * UDMAP section of the TRM for more information on this setting.  The specified
 * free queue must be valid within the Navigator Subsystem and must be owned
 * by the host, or a subordinate of the host, requesting allocation and
 * configuration of the receive flow.
 *
 * @rx_fdq3_qnum:
 * UDMAP receive flow free descriptor queue 3 configuration to be programmed
 * into the rx_fdq3_qnum field of the flow's RFLOW_RFE register.  See the
 * UDMAP section of the TRM for more information on this setting.  The specified
 * free queue must be valid within the Navigator Subsystem and must be owned
 * by the host, or a subordinate of the host, requesting allocation and
 * configuration of the receive flow.
 *
 * @rx_ps_location:
 * UDMAP receive flow PS words location configuration to be programmed into the
 * rx_ps_location field of the flow's RFLOW_RFA register.
 */
struct ti_sci_msg_rm_udmap_flow_cfg {
	u32 valid_params;
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_EINFO_PRESENT_VALID	BIT(0)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_PSINFO_PRESENT_VALID     BIT(1)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_ERROR_HANDLING_VALID     BIT(2)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_DESC_TYPE_VALID          BIT(3)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_SOP_OFFSET_VALID         BIT(4)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_DEST_QNUM_VALID          BIT(5)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_SRC_TAG_HI_VALID         BIT(6)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_SRC_TAG_LO_VALID         BIT(7)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_DEST_TAG_HI_VALID        BIT(8)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_DEST_TAG_LO_VALID        BIT(9)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_SRC_TAG_HI_SEL_VALID     BIT(10)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_SRC_TAG_LO_SEL_VALID     BIT(11)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_DEST_TAG_HI_SEL_VALID    BIT(12)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_DEST_TAG_LO_SEL_VALID    BIT(13)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_FDQ0_SZ0_QNUM_VALID      BIT(14)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_FDQ1_QNUM_VALID          BIT(15)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_FDQ2_QNUM_VALID          BIT(16)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_FDQ3_QNUM_VALID          BIT(17)
#define TI_SCI_MSG_VALUE_RM_UDMAP_FLOW_PS_LOCATION_VALID        BIT(18)
	u16 nav_id;
	u16 flow_index;
	u8 rx_einfo_present;
	u8 rx_psinfo_present;
	u8 rx_error_handling;
	u8 rx_desc_type;
	u16 rx_sop_offset;
	u16 rx_dest_qnum;
	u8 rx_src_tag_hi;
	u8 rx_src_tag_lo;
	u8 rx_dest_tag_hi;
	u8 rx_dest_tag_lo;
	u8 rx_src_tag_hi_sel;
	u8 rx_src_tag_lo_sel;
	u8 rx_dest_tag_hi_sel;
	u8 rx_dest_tag_lo_sel;
	u16 rx_fdq0_sz0_qnum;
	u16 rx_fdq1_qnum;
	u16 rx_fdq2_qnum;
	u16 rx_fdq3_qnum;
	u8 rx_ps_location;
};

/**
 * struct ti_sci_rm_udmap_ops - UDMA Management operations
 * @tx_ch_cfg: configure SoC Navigator Subsystem UDMA transmit channel.
 * @rx_ch_cfg: configure SoC Navigator Subsystem UDMA receive channel.
 * @rx_flow_cfg: configure SoC Navigator Subsystem UDMA receive flow.
 */
struct ti_sci_rm_udmap_ops {
	int (*tx_ch_cfg)(const struct ti_sci_handle *handle,
			 const struct ti_sci_msg_rm_udmap_tx_ch_cfg *params);
	int (*rx_ch_cfg)(const struct ti_sci_handle *handle,
			 const struct ti_sci_msg_rm_udmap_rx_ch_cfg *params);
	int (*rx_flow_cfg)(const struct ti_sci_handle *handle,
			   const struct ti_sci_msg_rm_udmap_flow_cfg *params);
};

/**
 * struct ti_sci_proc_ops - Processor Control operations
 * @request:	Request to control a physical processor. The requesting host
 *		should be in the processor access list
 * @release:	Relinquish a physical processor control
 * @handover:	Handover a physical processor control to another host
 *		in the permitted list
 * @set_config:	Set base configuration of a processor
 * @set_control: Setup limited control flags in specific cases
 * @get_status: Get the state of physical processor
 *
 * NOTE: The following paramteres are generic in nature for all these ops,
 * -handle:	Pointer to TI SCI handle as retrieved by *ti_sci_get_handle
 * -pid:	Processor ID
 * -hid:	Host ID
 */
struct ti_sci_proc_ops {
	int (*request)(const struct ti_sci_handle *handle, u8 pid);
	int (*release)(const struct ti_sci_handle *handle, u8 pid);
	int (*handover)(const struct ti_sci_handle *handle, u8 pid, u8 hid);
	int (*set_config)(const struct ti_sci_handle *handle, u8 pid,
			  u64 boot_vector, u32 cfg_set, u32 cfg_clr);
	int (*set_control)(const struct ti_sci_handle *handle, u8 pid,
			   u32 ctrl_set, u32 ctrl_clr);
	int (*get_status)(const struct ti_sci_handle *handle, u8 pid,
			  u64 *boot_vector, u32 *cfg_flags, u32 *ctrl_flags,
			  u32 *status_flags);
};

/**
 * struct ti_sci_ops - Function support for TI SCI
 * @dev_ops:	Device specific operations
 * @clk_ops:	Clock specific operations
 * @pm_ops:	Low Power Mode (LPM) control operations
 * @rm_core_ops:	Resource management core operations.
 * @rm_irq_ops:		IRQ management specific operations
 * @rm_ring_ops:	Ring Accelerator Management operations
 * @rm_psil_ops:	PSI-L thread operations
 * @rm_udmap_ops:	UDMA Management operations
 * @proc_ops:	Processor Control specific operations
 */
struct ti_sci_ops {
	struct ti_sci_dev_ops dev_ops;
	struct ti_sci_clk_ops clk_ops;
	struct ti_sci_pm_ops pm_ops;
	struct ti_sci_rm_core_ops rm_core_ops;
	struct ti_sci_rm_irq_ops rm_irq_ops;
	struct ti_sci_rm_ringacc_ops rm_ring_ops;
	struct ti_sci_rm_psil_ops rm_psil_ops;
	struct ti_sci_rm_udmap_ops rm_udmap_ops;
	struct ti_sci_proc_ops proc_ops;
};

/**
 * struct ti_sci_handle - Handle returned to TI SCI clients for usage.
 * @version:	structure containing version information
 * @ops:	operations that are made available to TI SCI clients
 */
struct ti_sci_handle {
	struct ti_sci_version_info version;
	struct ti_sci_ops ops;
};

#define TI_SCI_RESOURCE_NULL	0xffff

/**
 * struct ti_sci_resource - Structure representing a resource assigned
 *			    to a device.
 * @sets:	Number of sets available from this resource type
 * @lock:	Lock to guard the res map in each set.
 * @desc:	Array of resource descriptors.
 */
struct ti_sci_resource {
	u16 sets;
	raw_spinlock_t lock;
	struct ti_sci_resource_desc *desc;
};

#if IS_ENABLED(CONFIG_TI_SCI_PROTOCOL)
const struct ti_sci_handle *ti_sci_get_handle(struct device *dev);
int ti_sci_put_handle(const struct ti_sci_handle *handle);
const struct ti_sci_handle *devm_ti_sci_get_handle(struct device *dev);
const struct ti_sci_handle *ti_sci_get_by_phandle(struct device_node *np,
						  const char *property);
const struct ti_sci_handle *devm_ti_sci_get_by_phandle(struct device *dev,
						       const char *property);
u16 ti_sci_get_free_resource(struct ti_sci_resource *res);
void ti_sci_release_resource(struct ti_sci_resource *res, u16 id);
u32 ti_sci_get_num_resources(struct ti_sci_resource *res);
struct ti_sci_resource *
devm_ti_sci_get_of_resource(const struct ti_sci_handle *handle,
			    struct device *dev, u32 dev_id, char *of_prop);
struct ti_sci_resource *
devm_ti_sci_get_resource(const struct ti_sci_handle *handle, struct device *dev,
			 u32 dev_id, u32 sub_type);

#else	/* CONFIG_TI_SCI_PROTOCOL */

static inline const struct ti_sci_handle *ti_sci_get_handle(struct device *dev)
{
	return ERR_PTR(-EINVAL);
}

static inline int ti_sci_put_handle(const struct ti_sci_handle *handle)
{
	return -EINVAL;
}

static inline
const struct ti_sci_handle *devm_ti_sci_get_handle(struct device *dev)
{
	return ERR_PTR(-EINVAL);
}

static inline
const struct ti_sci_handle *ti_sci_get_by_phandle(struct device_node *np,
						  const char *property)
{
	return ERR_PTR(-EINVAL);
}

static inline
const struct ti_sci_handle *devm_ti_sci_get_by_phandle(struct device *dev,
						       const char *property)
{
	return ERR_PTR(-EINVAL);
}

static inline u16 ti_sci_get_free_resource(struct ti_sci_resource *res)
{
	return TI_SCI_RESOURCE_NULL;
}

static inline void ti_sci_release_resource(struct ti_sci_resource *res, u16 id)
{
}

static inline u32 ti_sci_get_num_resources(struct ti_sci_resource *res)
{
	return 0;
}

static inline struct ti_sci_resource *
devm_ti_sci_get_of_resource(const struct ti_sci_handle *handle,
			    struct device *dev, u32 dev_id, char *of_prop)
{
	return ERR_PTR(-EINVAL);
}

static inline struct ti_sci_resource *
devm_ti_sci_get_resource(const struct ti_sci_handle *handle, struct device *dev,
			 u32 dev_id, u32 sub_type)
{
	return ERR_PTR(-EINVAL);
}
#endif	/* CONFIG_TI_SCI_PROTOCOL */

#endif	/* __TISCI_PROTOCOL_H */
