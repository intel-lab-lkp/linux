/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2024 Intel Corporation. */

#ifndef _IAVF_PTP_H_
#define _IAVF_PTP_H_

#include <linux/ptp_clock_kernel.h>

/* structure used to queue PTP commands for processing */
struct iavf_ptp_aq_cmd {
	struct list_head list;
	enum virtchnl_ops v_opcode;
	u16 msglen;
	u8 msg[];
};

/* fields used for PTP support */
struct iavf_ptp {
	wait_queue_head_t phc_time_waitqueue;
	struct virtchnl_ptp_caps hw_caps;
	struct list_head aq_cmds;
	/* Lock protecting access to the AQ command list */
	spinlock_t aq_cmd_lock;
	u64 cached_phc_time;
	unsigned long cached_phc_updated;
	bool initialized;
	bool phc_time_ready;
	struct ptp_clock_info info;
	struct ptp_clock *clock;
};

void iavf_ptp_init(struct iavf_adapter *adapter);
void iavf_ptp_release(struct iavf_adapter *adapter);
void iavf_ptp_process_caps(struct iavf_adapter *adapter);
bool iavf_ptp_cap_supported(struct iavf_adapter *adapter, u32 cap);
void iavf_virtchnl_send_ptp_cmd(struct iavf_adapter *adapter);

#endif /* _IAVF_PTP_H_ */
