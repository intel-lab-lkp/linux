// SPDX-License-Identifier: GPL-2.0+
/*
 * Marvell PTP driver for 88E1510, 88E1512, 88E1514 and 88E1518 PHYs
 *
 * Ideas taken from 88E6xxx DSA and DP83640 drivers. This file
 * implements the packet timestamping support only (PTP).  TAI
 * support is separate.
 */
#include <linux/marvell_ptp.h>
#include <linux/netdevice.h>
#include <linux/phy.h>

#include "marvell_ptp.h"

#define MARVELL_PAGE_MISC			6
#define GCR					20
#define GCR_PTP_POWER_DOWN			BIT(9)
#define GCR_PTP_REF_CLOCK_SOURCE		BIT(8)
#define GCR_PTP_INPUT_SOURCE			BIT(7)
#define GCR_PTP_OUTPUT				BIT(6)

#define MARVELL_PAGE_PTP_PORT_1			8

#define MARVELL_PAGE_TAI_GLOBAL			12
#define MARVELL_PAGE_PTP_GLOBAL			14
#define PTPG_READPLUS_COMMAND			14
#define PTPG_READPLUS_DATA			15

struct marvell_phy_ptp {
	struct marvell_ptp ptp;
	struct mii_timestamper mii_ts;
};

static struct marvell_phy_ptp *mii_ts_to_phy_ptp(struct mii_timestamper *mii_ts)
{
	return container_of(mii_ts, struct marvell_phy_ptp, mii_ts);
}

static bool marvell_phy_ptp_rxtstamp(struct mii_timestamper *mii_ts,
				     struct sk_buff *skb, int type)
{
	struct marvell_phy_ptp *phy_ptp = mii_ts_to_phy_ptp(mii_ts);

	return marvell_ptp_rxtstamp(&phy_ptp->ptp, skb, type);
}

static void marvell_phy_ptp_txtstamp(struct mii_timestamper *mii_ts,
				     struct sk_buff *skb, int type)
{
	struct marvell_phy_ptp *phy_ptp = mii_ts_to_phy_ptp(mii_ts);

	return marvell_ptp_txtstamp(&phy_ptp->ptp, skb, type);
}

static int marvell_phy_ptp_hwtstamp(struct mii_timestamper *mii_ts,
				    struct kernel_hwtstamp_config *kcfg,
				    struct netlink_ext_ack *ack)
{
	struct marvell_phy_ptp *phy_ptp = mii_ts_to_phy_ptp(mii_ts);

	return marvell_ptp_hwtstamp(&phy_ptp->ptp, kcfg, ack);
}

static int marvell_phy_ptp_ts_info(struct mii_timestamper *mii_ts,
				   struct kernel_ethtool_ts_info *ts_info)
{
	struct marvell_phy_ptp *phy_ptp = mii_ts_to_phy_ptp(mii_ts);

	return marvell_ptp_ts_info(&phy_ptp->ptp, ts_info);
}

/* TAI accessor functions */
static int marvell_phy_tai_enable(struct device *dev)
{
	struct phy_device *phydev = to_phy_device(dev);

	return phy_modify_paged(phydev, MARVELL_PAGE_MISC, GCR,
				GCR_PTP_POWER_DOWN, 0);
}

static u64 marvell_phy_tai_clock_read(struct device *dev,
				      struct ptp_system_timestamp *sts)
{
	struct phy_device *phydev = to_phy_device(dev);
	int err, oldpage, lo, hi;

	oldpage = phy_select_page(phydev, MARVELL_PAGE_PTP_GLOBAL);
	if (oldpage >= 0) {
		/* 88e151x says to write 0x8e0e */
		ptp_read_system_prets(sts);
		err = __phy_write(phydev, PTPG_READPLUS_COMMAND, 0x8e0e);
		ptp_read_system_postts(sts);
		lo = __phy_read(phydev, PTPG_READPLUS_DATA);
		hi = __phy_read(phydev, PTPG_READPLUS_DATA);
	}
	err = phy_restore_page(phydev, oldpage, err);

	if (err || lo < 0 || hi < 0)
		return 0;

	return lo | hi << 16;
}

static int marvell_phy_tai_write(struct device *dev, u8 reg, u16 val)
{
	struct phy_device *phydev = to_phy_device(dev);

	return phy_write_paged(phydev, MARVELL_PAGE_TAI_GLOBAL, reg, val);
}

static int marvell_phy_tai_modify(struct device *dev, u8 reg, u16 mask, u16 val)
{
	struct phy_device *phydev = to_phy_device(dev);

	return phy_modify_paged(phydev, MARVELL_PAGE_TAI_GLOBAL,
				reg, mask, val);
}

static int marvell_phy_ptp_global_write(struct device *dev, u8 reg, u16 val)
{
	struct phy_device *phydev = to_phy_device(dev);

	return phy_write_paged(phydev, MARVELL_PAGE_PTP_GLOBAL, reg, val);
}

/* Read the status, timestamp and PTP common header sequence from the PHY.
 * Apparently, reading these are atomic, but there is no mention how the
 * PHY treats this access as atomic. So, we set the DisTSOverwrite bit
 * when configuring the PHY.
 */
static int marvell_phy_ptp_port_read_ts(struct device *dev,
					struct marvell_ts *ts, u8 reg)
{
	struct phy_device *phydev = to_phy_device(dev);
	int oldpage, page = MARVELL_PAGE_PTP_PORT_1 + (reg >> 4);
	int ret;

	reg &= 15;

	/* Read status register */
	oldpage = phy_select_page(phydev, page);
	if (oldpage >= 0) {
		ret = __phy_read(phydev, reg);
		if (ret < 0)
			goto restore;

		ts->stat = ret;
		if (!(ts->stat & MV_STATUS_VALID)) {
			ret = 0;
			goto restore;
		}

		/* Read low timestamp */
		ret = __phy_read(phydev, reg + 1);
		if (ret < 0)
			goto restore;

		ts->time = ret;

		/* Read high timestamp */
		ret = __phy_read(phydev, reg + 2);
		if (ret < 0)
			goto restore;

		ts->time |= ret << 16;

		/* Read sequence */
		ret = __phy_read(phydev, reg + 3);
		if (ret < 0)
			goto restore;

		ts->seq = ret;

		/* Clear valid */
		__phy_write(phydev, reg, 0);

		ret = 1;
	}
restore:
	return phy_restore_page(phydev, oldpage, ret);
}

static int marvell_phy_ptp_port_write(struct device *dev, u8 reg, u16 val)
{
	struct phy_device *phydev = to_phy_device(dev);

	return phy_write_paged(phydev, MARVELL_PAGE_PTP_PORT_1 + (reg >> 4),
			       reg & 15, val);
}

static int marvell_phy_ptp_port_modify(struct device *dev, u8 reg, u16 mask,
				       u16 val)
{
	struct phy_device *phydev = to_phy_device(dev);

	return phy_modify_paged(phydev, MARVELL_PAGE_PTP_PORT_1 + (reg >> 4),
				reg & 15, mask, val);
}

static long marvell_phy_ptp_aux_work(struct device *dev)
{
	struct phy_device *phydev = to_phy_device(dev);
	struct marvell_phy_ptp *phy_ptp;

	phy_ptp = mii_ts_to_phy_ptp(phydev->mii_ts);

	return marvell_ptp_aux_work(&phy_ptp->ptp);
}

static const struct marvell_ptp_ops marvell_phy_ptp_ops = {
	.tai_enable = marvell_phy_tai_enable,
	.tai_clock_read = marvell_phy_tai_clock_read,
	.tai_write = marvell_phy_tai_write,
	.tai_modify = marvell_phy_tai_modify,
	.ptp_global_write = marvell_phy_ptp_global_write,
	.ptp_port_read_ts = marvell_phy_ptp_port_read_ts,
	.ptp_port_write = marvell_phy_ptp_port_write,
	.ptp_port_modify = marvell_phy_ptp_port_modify,
	.ptp_aux_work = marvell_phy_ptp_aux_work,
};

static const struct marvell_tai_param marvell_phy_tai_param = {
	/* This assumes a 125MHz clock */
	.cc_mult_num = 1 << 9,
	.cc_mult_den = 15625U,
	.cc_mult = 8 << 28,
	.cc_shift = 28,
};

/* This function should be called from the PHY threaded interrupt
 * handler to process any stored timestamps in a timely manner.
 * The presence of an interrupt has an effect on how quickly a
 * timestamp requiring received packet will be processed.
 */
irqreturn_t marvell_phy_ptp_irq(struct phy_device *phydev)
{
	struct marvell_phy_ptp *phy_ptp;

	if (!phydev->mii_ts)
		return IRQ_NONE;

	phy_ptp = mii_ts_to_phy_ptp(phydev->mii_ts);

	return marvell_ptp_irq(&phy_ptp->ptp);
}
EXPORT_SYMBOL_GPL(marvell_phy_ptp_irq);

int marvell_phy_ptp_probe(struct phy_device *phydev)
{
	struct marvell_phy_ptp *phy_ptp;
	struct marvell_tai *tai;
	struct device *dev;
	int err;

	dev = &phydev->mdio.dev;

	phy_ptp = devm_kzalloc(dev, sizeof(*phy_ptp), GFP_KERNEL);
	if (!phy_ptp)
		return -ENOMEM;

	phy_ptp->mii_ts.rxtstamp = marvell_phy_ptp_rxtstamp;
	phy_ptp->mii_ts.txtstamp = marvell_phy_ptp_txtstamp;
	phy_ptp->mii_ts.hwtstamp = marvell_phy_ptp_hwtstamp;
	phy_ptp->mii_ts.ts_info = marvell_phy_ptp_ts_info;

	/* Get the TAI for this PHY. */
	err = marvell_tai_probe(&tai, &marvell_phy_ptp_ops,
				&marvell_phy_tai_param,
			        "Marvell PHY", dev);
	if (err)
		return err;

	err = marvell_ptp_probe(&phy_ptp->ptp, dev, tai,
				&marvell_phy_ptp_ops);
	if (err) {
		marvell_tai_remove(tai);
		return err;
	}

	phydev->mii_ts = &phy_ptp->mii_ts;

	return 0;
}
EXPORT_SYMBOL_GPL(marvell_phy_ptp_probe);

void marvell_phy_ptp_remove(struct phy_device *phydev)
{
	struct marvell_phy_ptp *phy_ptp;
	struct mii_timestamper *mii_ts;

	/* Disconnect from the net subsystem - we assume there is no
	 * packet activity at this point.
	 */
	mii_ts = phydev->mii_ts;
	phydev->mii_ts = NULL;

	if (mii_ts) {
		phy_ptp = mii_ts_to_phy_ptp(mii_ts);
		marvell_ptp_remove(&phy_ptp->ptp);
		marvell_tai_remove(phy_ptp->ptp.tai);
	}
}
EXPORT_SYMBOL_GPL(marvell_phy_ptp_remove);

MODULE_AUTHOR("Russell King");
MODULE_DESCRIPTION("Marvell PHY PTP library");
MODULE_LICENSE("GPL v2");
