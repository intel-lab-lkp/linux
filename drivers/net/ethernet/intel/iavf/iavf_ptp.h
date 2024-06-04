/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2024 Intel Corporation. */

#ifndef _IAVF_PTP_H_
#define _IAVF_PTP_H_

/* fields used for PTP support */
struct iavf_ptp {
	struct virtchnl_ptp_caps hw_caps;
};

#endif /* _IAVF_PTP_H_ */
