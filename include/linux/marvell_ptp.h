/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_MARVELL_PTP_H
#define LINUX_MARVELL_PTP_H

#include <linux/irqreturn.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/net_tstamp.h>
#include <linux/ptp_classify.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/skbuff.h>
#include <linux/timecounter.h>

struct device;
struct ifreq;
struct kernel_ethtool_ts_info;
struct marvell_tai;
struct netlink_ext_ack;

#define MV_PTP_MSGTYPE_DELAY_RESP	9

/* This defines which incoming or outgoing PTP frames are timestampped */
#define MV_PTP_MSD_ID_TS_EN	(BIT(PTP_MSGTYPE_SYNC) | \
				 BIT(PTP_MSGTYPE_DELAY_REQ) | \
				 BIT(MV_PTP_MSGTYPE_DELAY_RESP))
/* Direct Sync messages to Arr0 and delay messages to Arr1 */
#define MV_PTP_TS_ARR_PTR	(BIT(PTP_MSGTYPE_DELAY_REQ) | \
				 BIT(MV_PTP_MSGTYPE_DELAY_RESP))

struct marvell_extts {
	u32 time;
	u8 status;
#define MV_STATUS_EVENTCAPVALID	BIT(8)
};

struct marvell_ts {
	u32 time;
	u16 stat;
#define MV_STATUS_INTSTATUS_MASK	0x0006
#define MV_STATUS_INTSTATUS_NORMAL	0x0000
#define MV_STATUS_VALID			BIT(0)
	u16 seq;
};

struct marvell_ptp_ops {
	int (*tai_enable)(struct device *dev);
	u64 (*tai_clock_read)(struct device *dev,
			      struct ptp_system_timestamp *sts);
	int (*tai_extts_read)(struct device *dev, int reg,
			      struct marvell_extts *extts);
	int (*tai_pin_verify)(struct device *dev, int pin,
			      enum ptp_pin_function func, unsigned int chan);
	int (*tai_pin_setup)(struct device *dev, int pin, unsigned int flags,
			     int enable);
	int (*tai_write)(struct device *dev, u8 reg, u16 val);
	int (*tai_modify)(struct device *dev, u8 reg, u16 mask, u16 val);
	int (*ptp_global_write)(struct device *dev, u8 reg, u16 val);
	int (*ptp_port_read_ts)(struct device *dev, struct marvell_ts *ts,
			        u8 reg);
	int (*ptp_port_write)(struct device *dev, u8 reg, u16 val);
	int (*ptp_port_modify)(struct device *dev, u8 reg, u16 mask, u16 val);
	long (*ptp_aux_work)(struct device *dev);
};

/* TAI module */
struct marvell_tai_param {
	u32 cc_mult_num;
	u32 cc_mult_den;
	u32 cc_mult;
	int cc_shift;

	int n_ext_ts;
};

u64 marvell_tai_cyc2time(struct marvell_tai *tai, u32 cyc);
int marvell_tai_ptp_clock_index(struct marvell_tai *tai);
int marvell_tai_schedule(struct marvell_tai *tai, unsigned long delay);
int marvell_tai_probe(struct marvell_tai **taip,
		      const struct marvell_ptp_ops *ops,
		      const struct marvell_tai_param *param,
		      struct ptp_pin_desc *pin_config, int n_pins,
		      const char *name, struct device *dev);
void marvell_tai_remove(struct marvell_tai *tai);

/* Timestamping module */
struct marvell_rxts {
	struct list_head node;
	u64 ns;
	u16 seq;
};

struct marvell_rxq {
	struct mutex rx_mutex;
	struct list_head rx_free;
	struct list_head rx_pend;
	struct sk_buff_head rx_queue;
	struct marvell_rxts rx_ts[64];
};

struct marvell_ptp {
	struct marvell_tai *tai;
	const struct marvell_ptp_ops *ops;
	struct device *dev;

	/* We only support one outstanding transmit skb */
	struct sk_buff *tx_skb;
	enum hwtstamp_tx_types tx_type;

	struct marvell_rxq rxq[2];
	enum hwtstamp_rx_filters rx_filter;
};

bool marvell_ptp_rxtstamp(struct marvell_ptp *ptp, struct sk_buff *skb,
			  int type);
void marvell_ptp_txtstamp(struct marvell_ptp *ptp, struct sk_buff *skb,
			  int type);
int marvell_ptp_hwtstamp(struct marvell_ptp *ptp,
			 struct kernel_hwtstamp_config *kcfg,
			 struct netlink_ext_ack *ack);
int marvell_ptp_ts_info(struct marvell_ptp *ptp,
			struct kernel_ethtool_ts_info *ts_info);
long marvell_ptp_aux_work(struct marvell_ptp *ptp);
irqreturn_t marvell_ptp_irq(struct marvell_ptp *ptp);
int marvell_ptp_probe(struct marvell_ptp *ptp, struct device *dev,
		      struct marvell_tai *tai,
		      const struct marvell_ptp_ops *ops);
void marvell_ptp_remove(struct marvell_ptp *ptp);

#endif
