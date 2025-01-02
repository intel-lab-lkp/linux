/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2019 - 2025 Beijing WangXun Technology Co., Ltd. */

#ifndef _WX_PTP_H_
#define _WX_PTP_H_

void wx_ptp_start_cyclecounter(struct wx *wx);
void wx_ptp_reset(struct wx *wx);
void wx_ptp_init(struct wx *wx);
void wx_ptp_suspend(struct wx *wx);
void wx_ptp_stop(struct wx *wx);
void wx_ptp_overflow_check(struct wx *wx);
void wx_ptp_rx_hang(struct wx *wx);
void wx_ptp_tx_hang(struct wx *wx);
void wx_ptp_rx_hwtstamp(struct wx *wx, struct sk_buff *skb);
int wx_ptp_get_ts_config(struct wx *wx, struct ifreq *ifr);
int wx_ptp_set_ts_config(struct wx *wx, struct ifreq *ifr);

#endif /* _WX_PTP_H_ */
