/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2024 Intel Corporation. */

#ifndef _IAVF_PTP_H_
#define _IAVF_PTP_H_

#include <linux/ptp_clock_kernel.h>

/* bit indicating whether a 40bit timestamp is valid */
#define IAVF_PTP_40B_TSTAMP_VALID	BIT(0)

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
	struct kernel_hwtstamp_config hwtstamp_config;
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
long iavf_ptp_do_aux_work(struct ptp_clock_info *ptp);
int iavf_ptp_get_ts_config(struct iavf_adapter *adapter,
			   struct kernel_hwtstamp_config *config);
int iavf_ptp_set_ts_config(struct iavf_adapter *adapter,
			   struct kernel_hwtstamp_config *config,
			   struct netlink_ext_ack *extack);
u64 iavf_ptp_extend_32b_timestamp(u64 cached_phc_time, u32 in_tstamp);

#endif /* _IAVF_PTP_H_ */
