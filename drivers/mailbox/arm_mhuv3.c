// SPDX-License-Identifier: GPL-2.0
/*
 * ARM Message Handling Unit Version 3 (MHUv3) driver.
 *
 * Copyright (C) 2024 ARM Ltd.
 *
 * Based on ARM MHUv2 driver.
 */

#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/types.h>

/* ====== MHUv3 Registers ====== */

/* Maximum number of Doorbell channel windows */
#define MHUV3_DBCW_MAX			128
/* Number of DBCH combined interrupt status registers */
#define MHUV3_DBCH_CMB_INT_ST_REG_CNT	4
#define MHUV3_INVALID_DOORBELL		0xFFFFFFFFUL

/* Number of FFCH combined interrupt status registers */
#define MHUV3_FFCH_CMB_INT_ST_REG_CNT	2

#define MHUV3_STAT_BYTES		(sizeof(u32))
#define MHUV3_STAT_BITS			(MHUV3_STAT_BYTES * __CHAR_BIT__)

/* Not a typo ... */
#define MHUV3_MAJOR_VERSION		2

enum {
	MHUV3_MBOX_CELL_TYPE,
	MHUV3_MBOX_CELL_CHWN,
	MHUV3_MBOX_CELL_PARAM,
	MHUV3_MBOX_CELLS
};

/* CTRL_Page */

struct blk_id {
	u32 blk_id : 4;
	u32 pad : 28;
} __packed;

struct feat_spt0 {
	u32 dbe_spt : 4;
	u32 fe_spt : 4;
	u32 fce_spt : 4;
	u32 tze_spt : 4;
	u32 rme_spt : 4;
	u32 rase_spt : 4;
	u32 pad: 8;
} __packed;

struct feat_spt1 {
	u32 auto_op_spt : 4;
	u32 pad: 28;
} __packed;

struct dbch_cfg0 {
	u32 num_dbch : 8;
	u32 pad: 24;
} __packed;

struct ffch_cfg0 {
	u32 num_ffch : 8;
	u32 x8ba_spt : 1;
	u32 x16ba_spt : 1;
	u32 x32ba_spt : 1;
	u32 x64ba_spt : 1;
	u32 pad : 4;
	u32 ffch_depth : 10;
	u32 pad2 : 6;
} __packed;

struct fch_cfg0 {
	u32 num_fch : 10;
	/* MBX only registers */
	u32 fcgi_spt : 1;
	/* ------------------ */
	u32 num_fcg : 5;
	u32 num_fch_per_grp : 5;
	u32 fch_ws : 8;
	u32 pad : 3;
} __packed;

struct ctrl {
	u32 op_req : 1;
	u32 ch_op_mask : 1;
	u32 pad : 30;
} __packed;

struct fch_ctrl {
	u32 pad : 2;
	u32 int_en : 1;
	u32 pad2 : 29;
} __packed;

struct iidr {
	u32 implementer : 12;
	u32 revision : 4;
	u32 variant : 4;
	u32 product_id : 12;
} __packed;

struct aidr {
	u32 arch_minor_rev : 4;
	u32 arch_major_rev : 4;
	u32 pad : 24;
} __packed;

struct ctrl_page {
	struct blk_id blk_id;
	u8 pad[0x10 - 0x4];
	struct feat_spt0 feat_spt0;
	struct feat_spt1 feat_spt1;
	u8 pad1[0x20 - 0x18];
	struct dbch_cfg0 dbch_cfg0;
	u8 pad2[0x30 - 0x24];
	struct ffch_cfg0 ffch_cfg0;
	u8 pad3[0x40 - 0x34];
	struct fch_cfg0 fch_cfg0;
	u8 pad4[0x100 - 0x44];
	struct ctrl ctrl;
	/* MBX only registers */
	u8 pad5[0x140 - 0x104];
	struct fch_ctrl fch_ctrl;
	u32 fcg_int_en;
	u8 pad6[0x400 - 0x148];
	/* ------------------ */
	u32 dbch_int_st[MHUV3_DBCH_CMB_INT_ST_REG_CNT];
	u32 ffch_int_st[MHUV3_FFCH_CMB_INT_ST_REG_CNT];
	/* MBX only registers */
	u8 pad7[0x470 - 0x418];
	u32 fcg_int_st;
	u8 pad8[0x480 - 0x474];
	u32 fcg_grp_int_st[32];
	u8 pad9[0xFC8 - 0x500];
	/* ------------------ */
	struct iidr iidr;
	struct aidr aidr;
	u32 imp_def_id[12];
} __packed;

/* DBCW_Page */

struct xbcw_ctrl {
	u32 comb_en : 1;
	u32 pad : 31;
} __packed;

struct pdbcw_int {
	u32 tfr_ack : 1;
	u32 pad : 31;
} __packed;

struct pdbcw_page {
	u32 st;
	u8 pad[0xC - 0x4];
	u32 set;
	struct pdbcw_int int_st;
	struct pdbcw_int int_clr;
	struct pdbcw_int int_en;
	struct xbcw_ctrl ctrl;
} __packed;

struct mdbcw_page {
	u32 st;
	u32 st_msk;
	u32 clr;
	u8 pad[0x10 - 0xC];
	u32 msk_st;
	u32 msk_set;
	u32 msk_clr;
	struct xbcw_ctrl ctrl;
} __packed;

struct dummy_page {
	u8 pad[0x1000];
} __packed;

struct mhu3_pbx_frame_reg {
	struct ctrl_page ctrl;
	struct pdbcw_page dbcw[MHUV3_DBCW_MAX];
	struct dummy_page ffcw;
	struct dummy_page fcw;
	u8 pad[0xF000 - 0x4000];
	struct dummy_page impdef;
} __packed;

struct mhu3_mbx_frame_reg {
	struct ctrl_page ctrl;
	struct mdbcw_page dbcw[MHUV3_DBCW_MAX];
	struct dummy_page ffcw;
	struct dummy_page fcw;
	u8 pad[0xF000 - 0x4000];
	struct dummy_page impdef;
} __packed;

/* Macro for reading a bitfield within a physically mapped packed struct */
#define readl_relaxed_bitfield(_regptr, _field)				\
	({								\
		u32 _rval;						\
		typeof(_regptr) _rptr = _regptr;			\
		_rval = readl_relaxed(_rptr);				\
		((typeof(*_rptr) __force *)(&_rval))->_field;		\
	})

/* Macro for writing a bitfield within a physically mapped packed struct */
#define writel_relaxed_bitfield(_value, _regptr, _field)		\
	({								\
		u32 _rval;						\
		typeof(_regptr) _rptr = _regptr;			\
		_rval = readl_relaxed(_rptr);				\
		((typeof(*_rptr) __force *)(&_rval))->_field = _value;	\
		writel_relaxed(_rval, _rptr);				\
	})

/* ====== MHUv3 data structures ====== */

enum mhuv3_frame {
	PBX_FRAME,
	MBX_FRAME
};

static char *mhuv3_str[] = {
	"PBX",
	"MBX"
};

enum mhuv3_extension_type {
	FIRST_EXT = 0,
	DBE_EXT = FIRST_EXT,
	FCE_EXT,
	FE_EXT,
	MAX_EXT
};

struct mhuv3;

/**
 * struct mhuv3_protocol_ops - MHUv3 operations
 *
 * @rx_startup: Receiver startup callback.
 * @rx_shutdown: Receiver shutdown callback.
 * @read_data: Read available Sender in-band LE data (if any).
 * @rx_complete: Acknowledge data reception to the Sender. Any out-of-band data
 *		 has to have been already retrieved before calling this.
 * @tx_startup: Sender startup callback.
 * @tx_shutdown: Sender shutdown callback.
 * @last_tx_done: Report back to the Sender if the last transfer has completed.
 * @send_data: Send data to the receiver.
 *
 * Each supported transport protocol provides its own implementation of
 * these operations.
 */
struct mhuv3_protocol_ops {
	int (*rx_startup)(struct mhuv3 *mhu, struct mbox_chan *chan);
	void (*rx_shutdown)(struct mhuv3 *mhu, struct mbox_chan *chan);
	void *(*read_data)(struct mhuv3 *mhu, struct mbox_chan *chan);
	void (*rx_complete)(struct mhuv3 *mhu, struct mbox_chan *chan);
	void (*tx_startup)(struct mhuv3 *mhu, struct mbox_chan *chan);
	void (*tx_shutdown)(struct mhuv3 *mhu, struct mbox_chan *chan);
	int (*last_tx_done)(struct mhuv3 *mhu, struct mbox_chan *chan);
	int (*send_data)(struct mhuv3 *mhu, struct mbox_chan *chan, void *arg);
};

/**
 * struct mhuv3_mbox_chan_priv - MHUv3 channel private information
 *
 * @ch_idx: Channel window index associated to this mailbox channel.
 * @doorbell: Doorbell bit number within the @ch_idx window.
 *	      Only relevant to Doorbell transport.
 * @ops: Transport protocol specific operations for this channel.
 *
 * Transport specific data attached to mmailbox channel priv data.
 */
struct mhuv3_mbox_chan_priv {
	u32 ch_idx;
	u32 doorbell;
	const struct mhuv3_protocol_ops *ops;
};

/**
 * struct mhuv3_extension - MHUv3 extension descriptor
 *
 * @type: Type of extension
 * @max_chans: Max number of channels found for this extension.
 * @base_ch_idx: First channel number assigned to this extension, picked from
 *		 the set of all mailbox channels descriptors created.
 * @mbox_of_xlate: Extension specific helper to parse DT and lookup associated
 *		   channel from the related 'mboxes' property.
 * @combined_irq_setup: Extension specific helper to setup the combined irq.
 * @channels_init: Extension specific helper to initialize channels.
 * @chan_from_comb_irq_get: Extension specific helper to lookup which channel
 *			    triggered the combined irq.
 * @pending_db: Array of per-channel pending doorbells.
 * @pending_lock: Protect access to pending_db.
 */
struct mhuv3_extension {
	enum mhuv3_extension_type type;
	unsigned int max_chans;
	unsigned int base_ch_idx;
	struct mbox_chan *(*mbox_of_xlate)(struct mhuv3 *mhu,
					   unsigned int channel,
					   unsigned int param);
	void (*combined_irq_setup)(struct mhuv3 *mhu);
	int (*channels_init)(struct mhuv3 *mhu);
	struct mbox_chan *(*chan_from_comb_irq_get)(struct mhuv3 *mhu);
	u32 pending_db[MHUV3_DBCW_MAX];
	/* Protect access to pending_db */
	spinlock_t pending_lock;
};

/**
 * struct mhuv3 - MHUv3 mailbox controller data
 *
 * @frame:	Frame type: MBX_FRAME or PBX_FRAME.
 * @auto_op_full: Flag to indicate if the MHU supports AutoOp full mode.
 * @major: MHUv3 controller architectural major version.
 * @minor: MHUv3 controller architectural minor version.
 * @tot_chans: The total number of channnels discovered across all extensions.
 * @cmb_irq: Combined IRQ number if any found defined.
 * @ctrl: A reference to the MHUv3 control page for this block.
 * @pbx: Base address of the PBX register mapping region.
 * @mbx: Base address of the MBX register mapping region.
 * @ext: Array holding descriptors for any found implemented extension.
 * @mbox: Mailbox controller belonging to the MHU frame.
 */
struct mhuv3 {
	enum mhuv3_frame frame;
	bool auto_op_full;
	unsigned int major;
	unsigned int minor;
	unsigned int tot_chans;
	int cmb_irq;
	struct ctrl_page __iomem *ctrl;
	union {
		struct mhu3_pbx_frame_reg __iomem *pbx;
		struct mhu3_mbx_frame_reg __iomem *mbx;
	};
	struct mhuv3_extension *ext[MAX_EXT];
	struct mbox_controller mbox;
};

#define mhu_from_mbox(_mbox) container_of(_mbox, struct mhuv3, mbox)

typedef int (*mhuv3_extension_initializer)(struct mhuv3 *mhu);

/* =================== Doorbell transport protocol operations =============== */

static void mhuv3_doorbell_tx_startup(struct mhuv3 *mhu, struct mbox_chan *chan)
{
	struct mhuv3_mbox_chan_priv *priv = chan->con_priv;

	/* Enable Transfer Acknowledgment events */
	writel_relaxed_bitfield(0x1, &mhu->pbx->dbcw[priv->ch_idx].int_en, tfr_ack);
}

static void mhuv3_doorbell_tx_shutdown(struct mhuv3 *mhu, struct mbox_chan *chan)
{
	unsigned long flags;
	struct mhuv3_extension *e = mhu->ext[DBE_EXT];
	struct mhuv3_mbox_chan_priv *priv = chan->con_priv;

	/* Disable Channel Transfer Ack events */
	writel_relaxed_bitfield(0x0, &mhu->pbx->dbcw[priv->ch_idx].int_en, tfr_ack);

	/* Clear Channel Transfer Ack and pending doorbells */
	writel_relaxed_bitfield(0x1, &mhu->pbx->dbcw[priv->ch_idx].int_clr, tfr_ack);
	spin_lock_irqsave(&e->pending_lock, flags);
	e->pending_db[priv->ch_idx] = 0;
	spin_unlock_irqrestore(&e->pending_lock, flags);
}

static int mhuv3_doorbell_rx_startup(struct mhuv3 *mhu, struct mbox_chan *chan)
{
	struct mhuv3_mbox_chan_priv *priv = chan->con_priv;

	/* Unmask Channel Transfer events */
	writel_relaxed(BIT(priv->doorbell), &mhu->mbx->dbcw[priv->ch_idx].msk_clr);

	return 0;
}

static void mhuv3_doorbell_rx_shutdown(struct mhuv3 *mhu,
				       struct mbox_chan *chan)
{
	struct mhuv3_mbox_chan_priv *priv = chan->con_priv;

	/* Mask Channel Transfer events */
	writel_relaxed(BIT(priv->doorbell), &mhu->mbx->dbcw[priv->ch_idx].msk_set);
}

static void mhuv3_doorbell_rx_complete(struct mhuv3 *mhu, struct mbox_chan *chan)
{
	struct mhuv3_mbox_chan_priv *priv = chan->con_priv;

	/* Clearing the pending transfer generates the Channel Transfer Ack */
	writel_relaxed(BIT(priv->doorbell), &mhu->mbx->dbcw[priv->ch_idx].clr);
}

static int mhuv3_doorbell_last_tx_done(struct mhuv3 *mhu,
				       struct mbox_chan *chan)
{
	int done;
	struct mhuv3_mbox_chan_priv *priv = chan->con_priv;

	done = !(readl_relaxed(&mhu->pbx->dbcw[priv->ch_idx].st) &
		 BIT(priv->doorbell));
	if (done) {
		unsigned long flags;
		struct mhuv3_extension *e = mhu->ext[DBE_EXT];

		/* Take care to clear the pending doorbell also when polling */
		spin_lock_irqsave(&e->pending_lock, flags);
		e->pending_db[priv->ch_idx] &= ~BIT(priv->doorbell);
		spin_unlock_irqrestore(&e->pending_lock, flags);
	}

	return done;
}

static int mhuv3_doorbell_send_data(struct mhuv3 *mhu, struct mbox_chan *chan,
				    void *arg)
{
	int ret = 0;
	struct mhuv3_mbox_chan_priv *priv = chan->con_priv;
	struct mhuv3_extension *e = mhu->ext[DBE_EXT];
	unsigned long flags;

	spin_lock_irqsave(&e->pending_lock, flags);
	/* Only one in-flight Transfer is allowed per-doorbell */
	if (!(e->pending_db[priv->ch_idx] & BIT(priv->doorbell))) {
		e->pending_db[priv->ch_idx] |= BIT(priv->doorbell);
		writel_relaxed(BIT(priv->doorbell),
			       &mhu->pbx->dbcw[priv->ch_idx].set);
	} else {
		ret = -EBUSY;
	}
	spin_unlock_irqrestore(&e->pending_lock, flags);

	return ret;
}

static const struct mhuv3_protocol_ops mhuv3_doorbell_ops = {
	.tx_startup = mhuv3_doorbell_tx_startup,
	.tx_shutdown = mhuv3_doorbell_tx_shutdown,
	.rx_startup = mhuv3_doorbell_rx_startup,
	.rx_shutdown = mhuv3_doorbell_rx_shutdown,
	.rx_complete = mhuv3_doorbell_rx_complete,
	.last_tx_done = mhuv3_doorbell_last_tx_done,
	.send_data = mhuv3_doorbell_send_data,
};

/* Sender and receiver mailbox ops */
static bool mhuv3_sender_last_tx_done(struct mbox_chan *chan)
{
	struct mhuv3 *mhu = mhu_from_mbox(chan->mbox);
	struct mhuv3_mbox_chan_priv *priv = chan->con_priv;

	return priv->ops->last_tx_done(mhu, chan);
}

static int mhuv3_sender_send_data(struct mbox_chan *chan, void *data)
{
	struct mhuv3 *mhu = mhu_from_mbox(chan->mbox);
	struct mhuv3_mbox_chan_priv *priv = chan->con_priv;

	if (!priv->ops->last_tx_done(mhu, chan))
		return -EBUSY;

	return priv->ops->send_data(mhu, chan, data);
}

static int mhuv3_sender_startup(struct mbox_chan *chan)
{
	struct mhuv3 *mhu = mhu_from_mbox(chan->mbox);
	struct mhuv3_mbox_chan_priv *priv = chan->con_priv;

	if (priv->ops->tx_startup)
		priv->ops->tx_startup(mhu, chan);

	return 0;
}

static void mhuv3_sender_shutdown(struct mbox_chan *chan)
{
	struct mhuv3 *mhu = mhu_from_mbox(chan->mbox);
	struct mhuv3_mbox_chan_priv *priv = chan->con_priv;

	if (priv->ops->tx_shutdown)
		priv->ops->tx_shutdown(mhu, chan);
}

static const struct mbox_chan_ops mhuv3_sender_ops = {
	.send_data = mhuv3_sender_send_data,
	.startup = mhuv3_sender_startup,
	.shutdown = mhuv3_sender_shutdown,
	.last_tx_done = mhuv3_sender_last_tx_done,
};

static int mhuv3_receiver_startup(struct mbox_chan *chan)
{
	struct mhuv3 *mhu = mhu_from_mbox(chan->mbox);
	struct mhuv3_mbox_chan_priv *priv = chan->con_priv;

	return priv->ops->rx_startup(mhu, chan);
}

static void mhuv3_receiver_shutdown(struct mbox_chan *chan)
{
	struct mhuv3 *mhu = mhu_from_mbox(chan->mbox);
	struct mhuv3_mbox_chan_priv *priv = chan->con_priv;

	priv->ops->rx_shutdown(mhu, chan);
}

static int mhuv3_receiver_send_data(struct mbox_chan *chan, void *data)
{
	dev_err(chan->mbox->dev,
		"Trying to transmit on a MBX MHUv3 frame\n");
	return -EIO;
}

static bool mhuv3_receiver_last_tx_done(struct mbox_chan *chan)
{
	dev_err(chan->mbox->dev, "Trying to Tx poll on a MBX MHUv3 frame\n");
	return true;
}

static const struct mbox_chan_ops mhuv3_receiver_ops = {
	.send_data = mhuv3_receiver_send_data,
	.startup = mhuv3_receiver_startup,
	.shutdown = mhuv3_receiver_shutdown,
	.last_tx_done = mhuv3_receiver_last_tx_done,
};

static struct mbox_chan *mhuv3_dbe_mbox_of_xlate(struct mhuv3 *mhu,
						 unsigned int channel,
						 unsigned int doorbell)
{
	struct mbox_controller *mbox = &mhu->mbox;
	struct mbox_chan *chans = mbox->chans;
	struct mhuv3_extension *e = mhu->ext[DBE_EXT];

	if (channel >= e->max_chans || doorbell >= MHUV3_STAT_BITS) {
		dev_err(mbox->dev, "Couldn't xlate to a valid channel (%d: %d)\n",
			channel, doorbell);
		return ERR_PTR(-ENODEV);
	}

	return &chans[e->base_ch_idx + channel * MHUV3_STAT_BITS + doorbell];
}

static void mhuv3_dbe_combined_irq_setup(struct mhuv3 *mhu)
{
	int i;
	struct mhuv3_extension *e = mhu->ext[DBE_EXT];

	if (mhu->frame == PBX_FRAME) {
		struct pdbcw_page __iomem *dbcw = mhu->pbx->dbcw;

		for (i = 0; i < e->max_chans; i++) {
			writel_relaxed_bitfield(0x1, &dbcw[i].int_clr, tfr_ack);
			writel_relaxed_bitfield(0x0, &dbcw[i].int_en, tfr_ack);
			writel_relaxed_bitfield(0x1, &dbcw[i].ctrl, comb_en);
		}
	} else {
		struct mdbcw_page __iomem *dbcw = mhu->mbx->dbcw;

		for (i = 0; i < e->max_chans; i++) {
			writel_relaxed(0xFFFFFFFF, &dbcw[i].clr);
			writel_relaxed(0xFFFFFFFF, &dbcw[i].msk_set);
			writel_relaxed_bitfield(0x1, &dbcw[i].ctrl, comb_en);
		}
	}
}

static int mhuv3_dbe_channels_init(struct mhuv3 *mhu)
{
	int i;
	struct mhuv3_extension *e = mhu->ext[DBE_EXT];
	struct mbox_controller *mbox = &mhu->mbox;
	struct mbox_chan *chans;

	chans = mbox->chans + mbox->num_chans;
	e->base_ch_idx = mbox->num_chans;
	for (i = 0; i < e->max_chans; i++) {
		int k;
		struct mhuv3_mbox_chan_priv *priv;

		for (k = 0; k < MHUV3_STAT_BITS; k++) {
			priv = devm_kmalloc(mbox->dev, sizeof(*priv), GFP_KERNEL);
			if (!priv)
				return -ENOMEM;

			priv->ch_idx = i;
			priv->ops = &mhuv3_doorbell_ops;
			priv->doorbell = k;
			chans++->con_priv = priv;
			mbox->num_chans++;
		}
	}

	spin_lock_init(&e->pending_lock);

	return 0;
}

static struct mbox_chan *mhuv3_dbe_chan_from_comb_irq_get(struct mhuv3 *mhu)
{
	int i;
	struct mhuv3_extension *e = mhu->ext[DBE_EXT];
	struct device *dev = mhu->mbox.dev;

	for (i = 0; i < MHUV3_DBCH_CMB_INT_ST_REG_CNT; i++) {
		unsigned int channel, db = MHUV3_INVALID_DOORBELL;
		u32 cmb_st, st;

		cmb_st = readl_relaxed(&mhu->ctrl->dbch_int_st[i]);
		if (!cmb_st)
			continue;

		channel = i * MHUV3_STAT_BITS + __builtin_ctz(cmb_st);
		if (channel >= e->max_chans) {
			dev_err(dev, "Invalid %s channel:%d\n",
				mhuv3_str[mhu->frame], channel);
			break;
		}

		if (mhu->frame == PBX_FRAME) {
			unsigned long flags;
			u32 active_dbs, fired_dbs;

			st = readl_relaxed_bitfield(&mhu->pbx->dbcw[channel].int_st,
						    tfr_ack);
			if (!st) {
				dev_warn(dev, "Spurios IRQ on %s channel:%d\n",
					 mhuv3_str[mhu->frame], channel);
				continue;
			}

			active_dbs = readl_relaxed(&mhu->pbx->dbcw[channel].st);
			spin_lock_irqsave(&e->pending_lock, flags);
			fired_dbs = e->pending_db[channel] & ~active_dbs;
			if (fired_dbs) {
				db = __builtin_ctz(fired_dbs);
				e->pending_db[channel] &= ~BIT(db);
				fired_dbs &= ~BIT(db);
			}
			spin_unlock_irqrestore(&e->pending_lock, flags);

			/* Clear TFR Ack if no more doorbells pending */
			if (!fired_dbs)
				writel_relaxed_bitfield(0x1,
							&mhu->pbx->dbcw[channel].int_clr,
							tfr_ack);
		} else {
			st = readl_relaxed(&mhu->mbx->dbcw[channel].st_msk);
			if (!st) {
				dev_warn(dev, "Spurios IRQ on %s channel:%d\n",
					 mhuv3_str[mhu->frame], channel);
				continue;
			}
			db = __builtin_ctz(st);
		}

		if (db != MHUV3_INVALID_DOORBELL) {
			dev_dbg(dev, "Found %s ch[%d]/db[%d]\n",
				mhuv3_str[mhu->frame], channel, db);

			return &mhu->mbox.chans[channel * MHUV3_STAT_BITS + db];
		}
	}

	return ERR_PTR(-EIO);
}

static int mhuv3_dbe_init(struct mhuv3 *mhu)
{
	struct mhuv3_extension *e;
	struct device *dev = mhu->mbox.dev;

	if (!readl_relaxed_bitfield(&mhu->ctrl->feat_spt0, dbe_spt))
		return 0;

	dev_dbg(dev, "%s: Initializing DBE Extension.\n", mhuv3_str[mhu->frame]);

	e = devm_kzalloc(dev, sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	e->type = DBE_EXT;
	/* Note that, by the spec, the number of channels is (num_dbch + 1) */
	e->max_chans =
		readl_relaxed_bitfield(&mhu->ctrl->dbch_cfg0, num_dbch) + 1;
	e->mbox_of_xlate = mhuv3_dbe_mbox_of_xlate;
	e->combined_irq_setup = mhuv3_dbe_combined_irq_setup;
	e->channels_init = mhuv3_dbe_channels_init;
	e->chan_from_comb_irq_get = mhuv3_dbe_chan_from_comb_irq_get;

	mhu->tot_chans += e->max_chans * MHUV3_STAT_BITS;
	mhu->ext[DBE_EXT] = e;

	dev_info(dev, "%s: found %d DBE channels.\n",
		 mhuv3_str[mhu->frame], e->max_chans);

	return 0;
}

static int mhuv3_fce_init(struct mhuv3 *mhu)
{
	struct device *dev = mhu->mbox.dev;

	if (!readl_relaxed_bitfield(&mhu->ctrl->feat_spt0, fce_spt))
		return 0;

	dev_dbg(dev, "%s: FCE Extension not supported by driver.\n",
		mhuv3_str[mhu->frame]);

	return 0;
}

static int mhuv3_fe_init(struct mhuv3 *mhu)
{
	struct device *dev = mhu->mbox.dev;

	if (!readl_relaxed_bitfield(&mhu->ctrl->feat_spt0, fe_spt))
		return 0;

	dev_dbg(dev, "%s: FE Extension not supported by driver.\n",
		mhuv3_str[mhu->frame]);

	return 0;
}

static mhuv3_extension_initializer mhuv3_extension_init[MAX_EXT] = {
	mhuv3_dbe_init,
	mhuv3_fce_init,
	mhuv3_fe_init,
};

static int mhuv3_initialize_channels(struct device *dev, struct mhuv3 *mhu)
{
	int i, ret = 0;
	struct mbox_controller *mbox = &mhu->mbox;

	mbox->chans = devm_kcalloc(dev, mhu->tot_chans,
				   sizeof(*mbox->chans), GFP_KERNEL);
	if (!mbox->chans)
		return -ENOMEM;

	for (i = FIRST_EXT; i < MAX_EXT && !ret; i++)
		if (mhu->ext[i])
			ret = mhu->ext[i]->channels_init(mhu);

	return ret;
}

static struct mbox_chan *mhuv3_mbox_of_xlate(struct mbox_controller *mbox,
					     const struct of_phandle_args *pa)
{
	unsigned int type, channel, param;
	struct mhuv3 *mhu = mhu_from_mbox(mbox);

	if (pa->args_count != MHUV3_MBOX_CELLS)
		return ERR_PTR(-EINVAL);

	type = pa->args[MHUV3_MBOX_CELL_TYPE];
	if (type >= MAX_EXT)
		return ERR_PTR(-EINVAL);

	channel = pa->args[MHUV3_MBOX_CELL_CHWN];
	param = pa->args[MHUV3_MBOX_CELL_PARAM];

	return mhu->ext[type]->mbox_of_xlate(mhu, channel, param);
}

static int mhuv3_frame_init(struct mhuv3 *mhu, void __iomem *regs)
{
	int i, ret = 0;
	struct device *dev = mhu->mbox.dev;

	mhu->ctrl = regs;
	mhu->frame = readl_relaxed_bitfield(&mhu->ctrl->blk_id, blk_id);
	if (mhu->frame > MBX_FRAME) {
		dev_err(dev, "Invalid Frame type- %d\n", mhu->frame);
		return -EINVAL;
	}

	mhu->major = readl_relaxed_bitfield(&mhu->ctrl->aidr, arch_major_rev);
	mhu->minor = readl_relaxed_bitfield(&mhu->ctrl->aidr, arch_minor_rev);
	if (mhu->major != MHUV3_MAJOR_VERSION) {
		dev_warn(dev, "Unsupported MHU %s block - major:%d  minor:%d\n",
			 mhuv3_str[mhu->frame], mhu->major, mhu->minor);
		return -EINVAL;
	}
	mhu->auto_op_full = !!readl_relaxed_bitfield(&mhu->ctrl->feat_spt1,
						     auto_op_spt);
	/* Request the PBX/MBX to remain operational */
	if (mhu->auto_op_full)
		writel_relaxed_bitfield(0x1, &mhu->ctrl->ctrl, op_req);

	dev_dbg(dev, "Found MHU %s block - major:%d  minor:%d\n",
		mhuv3_str[mhu->frame], mhu->major, mhu->minor);

	if (mhu->frame == PBX_FRAME)
		mhu->pbx = regs;
	else
		mhu->mbx = regs;

	for (i = FIRST_EXT; i < MAX_EXT && !ret; i++)
		ret = mhuv3_extension_init[i](mhu);

	return ret;
}

static irqreturn_t mhuv3_pbx_comb_interrupt(int irq, void *arg)
{
	int ret = IRQ_NONE;
	unsigned int i, found = 0;
	struct mhuv3 *mhu = arg;
	struct device *dev = mhu->mbox.dev;
	struct mbox_chan *chan;

	for (i = FIRST_EXT; i < MAX_EXT; i++) {
		/* FCE does not participate to the PBX combined */
		if (i == FCE_EXT || !mhu->ext[i])
			continue;

		chan = mhu->ext[i]->chan_from_comb_irq_get(mhu);
		if (!IS_ERR(chan)) {
			struct mhuv3_mbox_chan_priv *priv = chan->con_priv;

			found++;
			if (chan->cl) {
				mbox_chan_txdone(chan, 0);
				ret = IRQ_HANDLED;
			} else {
				dev_warn(dev,
					 "TX Ack on UNBOUND channel (%u)\n",
					 priv->ch_idx);
			}
		}
	}

	if (!found)
		dev_warn_once(dev, "Failed to find channel for the TX interrupt\n");

	return ret;
}

static irqreturn_t mhuv3_mbx_comb_interrupt(int irq, void *arg)
{
	int ret = IRQ_NONE;
	unsigned int i, found = 0;
	struct mhuv3 *mhu = arg;
	struct device *dev = mhu->mbox.dev;
	struct mbox_chan *chan;

	for (i = FIRST_EXT; i < MAX_EXT; i++) {
		if (!mhu->ext[i])
			continue;

		/* Process any extension which could be source of the IRQ */
		chan = mhu->ext[i]->chan_from_comb_irq_get(mhu);
		if (!IS_ERR(chan)) {
			void *data = NULL;
			struct mhuv3_mbox_chan_priv *priv = chan->con_priv;

			found++;
			/* Read and acknowledge optional in-band LE data first. */
			if (priv->ops->read_data)
				data = priv->ops->read_data(mhu, chan);

			if (chan->cl && !IS_ERR(data)) {
				mbox_chan_received_data(chan, data);
				ret = IRQ_HANDLED;
			} else if (!chan->cl) {
				dev_warn(dev,
					 "RX Data on UNBOUND channel (%u)\n",
					 priv->ch_idx);
			} else {
				dev_err(dev, "Failed to read data: %lu\n",
					PTR_ERR(data));
			}

			if (!IS_ERR(data))
				kfree(data);

			/*
			 * Acknowledge transfer after any possible optional
			 * out-of-band data has also been retrieved via
			 * mbox_chan_received_data().
			 */
			if (priv->ops->rx_complete)
				priv->ops->rx_complete(mhu, chan);
		}
	}

	if (!found)
		dev_warn_once(dev, "Failed to find channel for the RX interrupt\n");

	return ret;
}

static int mhuv3_setup_pbx(struct mhuv3 *mhu)
{
	struct device *dev = mhu->mbox.dev;

	mhu->mbox.ops = &mhuv3_sender_ops;

	if (mhu->cmb_irq > 0) {
		int ret;

		ret = devm_request_threaded_irq(dev, mhu->cmb_irq, NULL,
						mhuv3_pbx_comb_interrupt,
						IRQF_ONESHOT, "mhuv3-pbx", mhu);
		if (!ret) {
			int i;

			mhu->mbox.txdone_irq = true;
			mhu->mbox.txdone_poll = false;

			for (i = FIRST_EXT; i < MAX_EXT; i++)
				if (mhu->ext[i])
					mhu->ext[i]->combined_irq_setup(mhu);

			dev_dbg(dev, "MHUv3 PBX IRQs initialized.\n");

			return 0;
		}

		dev_err(dev, "Failed to request PBX IRQ - ret:%d", ret);
	}

	dev_info(dev, "Using PBX in Tx polling mode.\n");
	mhu->mbox.txdone_irq = false;
	mhu->mbox.txdone_poll = true;
	mhu->mbox.txpoll_period = 1;

	return 0;
}

static int mhuv3_setup_mbx(struct mhuv3 *mhu)
{
	int ret, i;
	struct device *dev = mhu->mbox.dev;

	mhu->mbox.ops = &mhuv3_receiver_ops;

	if (mhu->cmb_irq <= 0) {
		dev_err(dev, "Missing MBX combined IRQ !\n");
		return -EINVAL;
	}

	ret = devm_request_threaded_irq(dev, mhu->cmb_irq, NULL,
					mhuv3_mbx_comb_interrupt, IRQF_ONESHOT,
					"mhuv3-mbx", mhu);
	if (ret) {
		dev_err(dev, "Failed to request MBX IRQ - ret:%d\n", ret);
		return ret;
	}

	for (i = FIRST_EXT; i < MAX_EXT; i++)
		if (mhu->ext[i])
			mhu->ext[i]->combined_irq_setup(mhu);

	dev_dbg(dev, "MHUv3 MBX IRQs initialized.\n");

	return ret;
}

static int mhuv3_irqs_init(struct mhuv3 *mhu, struct platform_device *pdev)
{
	int ret;

	dev_dbg(mhu->mbox.dev, "Initializing %s block.\n", mhuv3_str[mhu->frame]);

	if (mhu->frame == PBX_FRAME) {
		mhu->cmb_irq = platform_get_irq_byname_optional(pdev, "combined");
		ret = mhuv3_setup_pbx(mhu);
	} else {
		mhu->cmb_irq = platform_get_irq_byname(pdev, "combined");
		ret = mhuv3_setup_mbx(mhu);
	}

	return ret;
}

static int mhuv3_probe(struct platform_device *pdev)
{
	int ret;
	struct mhuv3 *mhu;
	void __iomem *regs;
	struct device *dev = &pdev->dev;

	mhu = devm_kzalloc(dev, sizeof(*mhu), GFP_KERNEL);
	if (!mhu)
		return -ENOMEM;

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	mhu->mbox.dev = dev;
	ret = mhuv3_frame_init(mhu, regs);
	if (ret)
		return ret;

	ret = mhuv3_irqs_init(mhu, pdev);
	if (ret)
		return ret;

	mhu->mbox.of_xlate = mhuv3_mbox_of_xlate;
	ret = mhuv3_initialize_channels(dev, mhu);
	if (ret)
		return ret;

	ret = devm_mbox_controller_register(dev, &mhu->mbox);
	if (ret)
		dev_err(dev, "failed to register ARM MHUv3 driver %d\n", ret);

	platform_set_drvdata(pdev, mhu);

	return ret;
}

static int mhuv3_remove(struct platform_device *pdev)
{
	struct mhuv3 *mhu = platform_get_drvdata(pdev);

	if (mhu->auto_op_full)
		writel_relaxed_bitfield(0x0, &mhu->ctrl->ctrl, op_req);

	return 0;
}

static const struct of_device_id mhuv3_of_match[] = {
	{ .compatible = "arm,mhuv3", .data = NULL },
	{}
};
MODULE_DEVICE_TABLE(of, mhuv3_of_match);

static struct platform_driver mhuv3_driver = {
	.driver = {
		.name = "arm-mhuv3-mailbox",
		.of_match_table = mhuv3_of_match,
	},
	.probe = mhuv3_probe,
	.remove = mhuv3_remove,
};
module_platform_driver(mhuv3_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ARM MHUv3 Driver");
MODULE_AUTHOR("Cristian Marussi <cristian.marussi@arm.com>");
