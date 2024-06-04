/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2024 Intel Corporation. */

#ifndef _IAVF_PTP_H_
#define _IAVF_PTP_H_

#include <linux/ptp_clock_kernel.h>

/* fields used for PTP support */
struct iavf_ptp {
	struct virtchnl_ptp_caps hw_caps;
	bool initialized;
	struct ptp_clock_info info;
	struct ptp_clock *clock;
};

void iavf_ptp_init(struct iavf_adapter *adapter);
void iavf_ptp_release(struct iavf_adapter *adapter);
void iavf_ptp_process_caps(struct iavf_adapter *adapter);
bool iavf_ptp_cap_supported(struct iavf_adapter *adapter, u32 cap);

#endif /* _IAVF_PTP_H_ */
