/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2024 Intel Corporation. */

#ifndef _IAVF_PTP_H_
#define _IAVF_PTP_H_

#include "iavf_types.h"

#define iavf_clock_to_adapter(info)				\
	container_of_const(info, struct iavf_adapter, ptp.info)

void iavf_ptp_init(struct iavf_adapter *adapter);
void iavf_ptp_release(struct iavf_adapter *adapter);
void iavf_ptp_process_caps(struct iavf_adapter *adapter);
bool iavf_ptp_cap_supported(const struct iavf_adapter *adapter, u32 cap);
void iavf_virtchnl_send_ptp_cmd(struct iavf_adapter *adapter);

#endif /* _IAVF_PTP_H_ */
