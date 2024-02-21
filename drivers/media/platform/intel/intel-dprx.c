// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2023-2024 Google LLC.
 * Author: Paweł Anikiel <panikiel@google.com>
 */

#include <linux/crc-dp.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <media/v4l2-dv-timings.h>
#include <media/v4l2-subdev.h>
#include <drm/display/drm_dp.h>
#include <drm/display/drm_dp_mst.h>

#define DPRX_MAX_EDID_BLOCKS	4

/* DPRX registers */

#define DPRX_RX_CONTROL				0x000
#define DPRX_RX_CONTROL_LINK_RATE_SHIFT		16
#define DPRX_RX_CONTROL_LINK_RATE_MASK		0xff
#define DPRX_RX_CONTROL_RECONFIG_LINKRATE	13
#define DPRX_RX_CONTROL_TP_SHIFT		8
#define DPRX_RX_CONTROL_TP_MASK			0x7
#define DPRX_RX_CONTROL_SCRAMBLER_DISABLE	7
#define DPRX_RX_CONTROL_CHANNEL_CODING_SHIFT	5
#define DPRX_RX_CONTROL_CHANNEL_CODING_8B10B	0x1
#define DPRX_RX_CONTROL_LANE_COUNT_SHIFT	0
#define DPRX_RX_CONTROL_LANE_COUNT_MASK		0x1f

#define DPRX_RX_STATUS				0x001
#define DPRX_RX_STATUS_INTERLANE_ALIGN		8
#define DPRX_RX_STATUS_SYM_LOCK_SHIFT		4
#define DPRX_RX_STATUS_SYM_LOCK(i)		(4 + i)
#define DPRX_RX_STATUS_CR_LOCK_SHIFT		0
#define DPRX_RX_STATUS_CR_LOCK(i)		(0 + i)

#define DPRX_MSA_HTOTAL(i)			(0x022 + 0x20 * (i))
#define DPRX_MSA_VTOTAL(i)			(0x023 + 0x20 * (i))
#define DPRX_MSA_HSP(i)				(0x024 + 0x20 * (i))
#define DPRX_MSA_HSW(i)				(0x025 + 0x20 * (i))
#define DPRX_MSA_HSTART(i)			(0x026 + 0x20 * (i))
#define DPRX_MSA_VSTART(i)			(0x027 + 0x20 * (i))
#define DPRX_MSA_VSP(i)				(0x028 + 0x20 * (i))
#define DPRX_MSA_VSW(i)				(0x029 + 0x20 * (i))
#define DPRX_MSA_HWIDTH(i)			(0x02a + 0x20 * (i))
#define DPRX_MSA_VHEIGHT(i)			(0x02b + 0x20 * (i))
#define DPRX_VBID(i)				(0x02f + 0x20 * (i))
#define DPRX_VBID_MSA_LOCK			7

#define DPRX_MST_CONTROL1			0x0a0
#define DPRX_MST_CONTROL1_VCPTAB_UPD_FORCE	31
#define DPRX_MST_CONTROL1_VCPTAB_UPD_REQ	30
#define DPRX_MST_CONTROL1_VCP_ID_SHIFT(i)	(4 + 4 * (i))
#define DPRX_MST_CONTROL1_VCP_IDS_SHIFT		4
#define DPRX_MST_CONTROL1_VCP_IDS_MASK		0xffff
#define DPRX_MST_CONTROL1_MST_EN		0

#define DPRX_MST_STATUS1			0x0a1
#define DPRX_MST_STATUS1_VCPTAB_ACT_ACK		30

#define DPRX_MST_VCPTAB(i)			(0x0a2 + i)

#define DPRX_AUX_CONTROL			0x100
#define DPRX_AUX_CONTROL_IRQ_EN			8
#define DPRX_AUX_CONTROL_TX_STROBE		7
#define DPRX_AUX_CONTROL_LENGTH_SHIFT		0
#define DPRX_AUX_CONTROL_LENGTH_MASK		0x1f

#define DPRX_AUX_STATUS				0x101
#define DPRX_AUX_STATUS_MSG_READY		31
#define DPRX_AUX_STATUS_READY_TO_TX		30

#define DPRX_AUX_COMMAND			0x102

#define DPRX_AUX_HPD				0x119
#define DPRX_AUX_HPD_IRQ			12
#define DPRX_AUX_HPD_EN				11

/* DDC defines */

#define DDC_EDID_ADDR		0x50
#define DDC_SEGMENT_ADDR	0x30

struct dprx_training_control {
	u8 volt_swing;
	u8 pre_emph;
	bool max_swing;
	bool max_pre_emph;
};

struct dprx_sink {
	u8 edid[128 * DPRX_MAX_EDID_BLOCKS];
	int blocks;
	int offset;
	int segment;
};

struct msg_transaction_rxbuf {
	u8 buf[256];
	int len;
};

struct msg_transaction_txbuf {
	u8 buf[256];
	int len;
	int written;
};

struct msg_transaction_meta {
	u8 lct;
	u8 rad[8];
	bool seqno;
};

struct dprx {
	struct device *dev;
	void __iomem *iobase;

	struct v4l2_subdev subdev;
	struct media_pad pads[4];

	struct dprx_sink sinks[4];

	int max_link_rate;
	int max_lane_count;
	bool multi_stream_support;
	int max_stream_count;

	u8 caps[16];
	u8 guid[16];

	struct dprx_training_control training_control[4];

	u8 payload_allocate_set;
	u8 payload_allocate_start_time_slot;
	u8 payload_allocate_time_slot_count;
	u8 payload_table[64];
	u8 payload_table_updated;

	u8  payload_id[4];
	u32 payload_pbn[4];
	u32 payload_pbn_total;

	u8 irq_vector;

	u8 down_req_buf[48];
	u8 down_rep_buf[48];

	struct msg_transaction_rxbuf mt_rxbuf[2];
	struct msg_transaction_txbuf mt_txbuf[2];
	struct msg_transaction_meta mt_meta[2];
	bool mt_seqno;
	bool mt_pending;
	bool down_rep_pending;

	spinlock_t lock;
};

struct aux_buf {
	u8 data[20];
	int len;
};

struct aux_msg {
	u8 cmd;
	u32 addr;
	u8 len;
	u8 data[16];
};

struct sideband_msg {
	u8 lct;
	u8 lcr;
	u8 rad[8];
	bool broadcast;
	bool path_msg;
	bool somt;
	bool eomt;
	bool seqno;

	u8 body[48];
	u8 body_len;
};

static void dprx_write(struct dprx *dprx, int addr, u32 val)
{
	writel(val, dprx->iobase + (addr * 4));
}

static u32 dprx_read(struct dprx *dprx, int addr)
{
	return readl(dprx->iobase + (addr * 4));
}

static void dprx_set_irq(struct dprx *dprx, int val)
{
	u32 reg;

	reg = dprx_read(dprx, DPRX_AUX_CONTROL);
	reg |= ~(1 << DPRX_AUX_CONTROL_IRQ_EN);
	reg |= val << DPRX_AUX_CONTROL_IRQ_EN;
	dprx_write(dprx, DPRX_AUX_CONTROL, reg);
}

static void dprx_set_hpd(struct dprx *dprx, int val)
{
	u32 reg;

	reg = dprx_read(dprx, DPRX_AUX_HPD);
	reg &= ~(1 << DPRX_AUX_HPD_EN);
	reg |= val << DPRX_AUX_HPD_EN;
	dprx_write(dprx, DPRX_AUX_HPD, reg);
}

static void dprx_pulse_hpd(struct dprx *dprx)
{
	u32 reg;

	reg = dprx_read(dprx, DPRX_AUX_HPD);
	reg |= 1 << DPRX_AUX_HPD_IRQ;
	dprx_write(dprx, DPRX_AUX_HPD, reg);
}

static void dprx_clear_vc_payload_table(struct dprx *dprx)
{
	u32 reg;
	int i;

	memset(dprx->payload_table, 0, sizeof(dprx->payload_table));

	for (i = 0; i < 8; i++)
		dprx_write(dprx, DPRX_MST_VCPTAB(i), 0);

	reg = dprx_read(dprx, DPRX_MST_CONTROL1);
	reg &= ~(DPRX_MST_CONTROL1_VCP_IDS_MASK << DPRX_MST_CONTROL1_VCP_IDS_SHIFT);
	reg |= 1 << DPRX_MST_CONTROL1_VCPTAB_UPD_FORCE;
	dprx_write(dprx, DPRX_MST_CONTROL1, reg);
}

static void dprx_set_vc_payload_table(struct dprx *dprx)
{
	int i, j;
	u32 reg;
	u8 val;

	/*
	 * The IP core only accepts VC payload IDs of 1-4. Thus, we need to
	 * remap the 1-63 range allowed by DisplayPort into 1-4. However, some
	 * hosts first set the VC payload table and then allocate the VC
	 * payload IDs, which means we can't remap the range immediately.
	 *
	 * It is probably possible to force a VC payload table update (without
	 * waiting for a ACT trigger) when the IDs change, but for now we just
	 * ignore IDs higher than 4.
	 */
	for (i = 0; i < 8; i++) {
		reg = 0;
		for (j = 0; j < 8; j++) {
			val = dprx->payload_table[i*8+j];
			if (val <= 4)
				reg |= val << (j * 4);
		}
		dprx_write(dprx, DPRX_MST_VCPTAB(i), reg);
	}

	reg = dprx_read(dprx, DPRX_MST_CONTROL1);
	reg |= 1 << DPRX_MST_CONTROL1_VCPTAB_UPD_REQ;
	dprx_write(dprx, DPRX_MST_CONTROL1, reg);
}

static void dprx_set_vc_ids(struct dprx *dprx)
{
	u32 reg;
	int i;

	reg = dprx_read(dprx, DPRX_MST_CONTROL1);
	reg &= ~(DPRX_MST_CONTROL1_VCP_IDS_MASK << DPRX_MST_CONTROL1_VCP_IDS_SHIFT);
	for (i = 0; i < dprx->max_stream_count; i++) {
		if (dprx->payload_id[i] <= 4)
			reg |= dprx->payload_id[i] << DPRX_MST_CONTROL1_VCP_ID_SHIFT(i);
	}
	dprx_write(dprx, DPRX_MST_CONTROL1, reg);
}

static void dprx_allocate_vc_payload(struct dprx *dprx, u8 start, u8 count, u8 id)
{
	if (count > sizeof(dprx->payload_table) - start)
		count = sizeof(dprx->payload_table) - start;
	memset(dprx->payload_table + start, id, count);
}

static void dprx_deallocate_vc_payload(struct dprx *dprx, int start, u8 id)
{
	u8 to = start;
	u8 i;

	for (i = start; i < sizeof(dprx->payload_table); i++) {
		if (dprx->payload_table[i] == id)
			dprx->payload_table[i] = 0;
		else
			dprx->payload_table[to++] = dprx->payload_table[i];
	}
}

static u32 dprx_full_pbn(struct dprx *dprx)
{
	u32 reg;
	u32 lane_count;
	u32 link_rate;

	if ((dprx_read(dprx, DPRX_RX_STATUS) >> DPRX_RX_STATUS_INTERLANE_ALIGN) & 1) {
		/* link training done - get current bandwidth */
		reg = dprx_read(dprx, DPRX_RX_CONTROL);
		lane_count = (reg >> DPRX_RX_CONTROL_LANE_COUNT_SHIFT) &
			     DPRX_RX_CONTROL_LANE_COUNT_MASK;
		link_rate  = (reg >> DPRX_RX_CONTROL_LINK_RATE_SHIFT) &
			     DPRX_RX_CONTROL_LINK_RATE_MASK;
	} else {
		/* link training not done - get max bandwidth */
		lane_count = dprx->max_lane_count;
		link_rate  = dprx->max_link_rate;
	}

	return lane_count * link_rate * 32;
}

static int dprx_port_number_to_sink_idx(struct dprx *dprx, u8 port_number)
{
	/* check if port number is valid */
	if (port_number < DP_MST_LOGICAL_PORT_0 ||
	    port_number >= DP_MST_LOGICAL_PORT_0 + dprx->max_stream_count)
		return -1;

	return port_number - DP_MST_LOGICAL_PORT_0;
}

static bool dprx_adjust_needed(struct dprx *dprx)
{
	u32 control;
	u32 status;
	u32 lane_count;
	u32 lane_count_mask;
	u32 pattern;

	control = dprx_read(dprx, DPRX_RX_CONTROL);
	status = dprx_read(dprx, DPRX_RX_STATUS);

	pattern = (control >> DPRX_RX_CONTROL_TP_SHIFT) & DPRX_RX_CONTROL_TP_MASK;
	lane_count = (control >> DPRX_RX_CONTROL_LANE_COUNT_SHIFT) &
		     DPRX_RX_CONTROL_LANE_COUNT_MASK;
	lane_count_mask = (1 << lane_count) - 1;

	if (pattern == 0) {
		/* link training not in progress */
		return false;
	} else if (pattern == 1) {
		/* link training CR phase - check CR lock */
		return (~status) & (lane_count_mask << DPRX_RX_STATUS_CR_LOCK_SHIFT);
	}
	/* link training EQ phase - check synbol lock and interlane align */
	return (~status) & (lane_count_mask << DPRX_RX_STATUS_SYM_LOCK_SHIFT |
			    1 << DPRX_RX_STATUS_INTERLANE_ALIGN);
}

/*
 * Return next allowed voltage swing, and pre-emphasis pair.
 * DisplayPort 1.2 spec, section 3.1.5.2
 */
static void dprx_training_control_next(struct dprx_training_control *ctl,
				       u8 *next_volt_swing, u8 *next_pre_emph)
{
	u8 volt_swing = ctl->volt_swing;
	u8 pre_emph = ctl->pre_emph;

	pre_emph++;
	if (pre_emph > 2) {
		volt_swing++;
		pre_emph = 0;
	}

	if (volt_swing > 2 || (volt_swing == 2 && pre_emph == 2)) {
		volt_swing = 0;
		pre_emph = 0;
	}

	*next_volt_swing = volt_swing;
	*next_pre_emph = pre_emph;
}

static int dprx_i2c_read(struct dprx_sink *sink, u8 addr, u8 *buf, int len)
{
	int offset;

	if (len == 0)
		return 0;

	switch (addr) {
	case DDC_EDID_ADDR:
		offset = sink->offset + sink->segment * 256;
		if (len + offset > sink->blocks * 128)
			return -1;
		memcpy(buf, sink->edid + offset, len);
		sink->offset += len;
		break;
	case DDC_SEGMENT_ADDR:
		if (len > 1)
			return -1;
		buf[0] = sink->segment;
		break;
	default:
		return -1;
	}

	return 0;
}

static int dprx_i2c_write(struct dprx_sink *sink, u8 addr, u8 *buf, int len)
{
	if (len == 0)
		return 0;
	if (len > 1)
		return -1;

	switch (addr) {
	case DDC_EDID_ADDR:
		sink->offset = buf[0];
		break;
	case DDC_SEGMENT_ADDR:
		sink->segment = buf[0];
		break;
	default:
		return -1;
	}

	return 0;
}

static void dprx_i2c_stop(struct dprx_sink *sink)
{
	sink->segment = 0;
}

static void dprx_write_nak(struct dprx *dprx,
			   struct drm_dp_sideband_msg_reply_body *rep,
			   u8 req_type, u8 reason)
{
	rep->reply_type = DP_SIDEBAND_REPLY_NAK;
	rep->req_type = req_type;

	memcpy(rep->u.nak.guid, dprx->guid, sizeof(dprx->guid));
	rep->u.nak.reason = reason;
	rep->u.nak.nak_data = 0;
}

static void dprx_execute_link_address(struct dprx *dprx,
				      struct drm_dp_sideband_msg_req_body *req,
				      struct drm_dp_sideband_msg_reply_body *rep)
{
	struct drm_dp_link_address_ack_reply *link_address = &rep->u.link_addr;
	struct drm_dp_link_addr_reply_port *port = link_address->ports;
	int i;

	rep->reply_type = DP_SIDEBAND_REPLY_ACK;
	rep->req_type = DP_LINK_ADDRESS;

	memcpy(link_address->guid, dprx->guid, sizeof(dprx->guid));
	link_address->nports = dprx->max_stream_count + 1;

	/* Port 0: input (physical) */
	port->input_port = true;
	port->peer_device_type = DP_PEER_DEVICE_SOURCE_OR_SST;
	port->port_number = 0;
	port->mcs = false;
	port->ddps = true;
	port++;

	for (i = 0; i < dprx->max_stream_count; i++) {
		/* Port 8 + n: internal sink number n (logical) */
		port->input_port = false;
		port->port_number = DP_MST_LOGICAL_PORT_0 + i;
		port->mcs = false;
		if (dprx->sinks[i].blocks > 0) {
			port->peer_device_type = DP_PEER_DEVICE_SST_SINK;
			port->ddps = true;
		} else {
			port->peer_device_type = DP_PEER_DEVICE_NONE;
			port->ddps = false;
		}
		port->legacy_device_plug_status = false;
		port->dpcd_revision = 0;
		memset(port->peer_guid, 0, 16);
		port->num_sdp_streams = 0;
		port->num_sdp_stream_sinks = 0;
		port++;
	}
}

static void dprx_execute_connection_status_notify(struct dprx *dprx,
						  struct drm_dp_sideband_msg_req_body *req,
						  struct drm_dp_sideband_msg_reply_body *rep)
{
	rep->reply_type = DP_SIDEBAND_REPLY_ACK;
	rep->req_type = DP_CONNECTION_STATUS_NOTIFY;
}

static void dprx_execute_enum_path_resources(struct dprx *dprx,
					     struct drm_dp_sideband_msg_req_body *req,
					     struct drm_dp_sideband_msg_reply_body *rep)
{
	u32 full_pbn = dprx_full_pbn(dprx);

	rep->reply_type = DP_SIDEBAND_REPLY_ACK;
	rep->req_type = DP_ENUM_PATH_RESOURCES;

	rep->u.path_resources.port_number = req->u.port_num.port_number;
	rep->u.path_resources.fec_capable = false;
	rep->u.path_resources.full_payload_bw_number = full_pbn;
	if (dprx->payload_pbn_total > full_pbn)
		rep->u.path_resources.avail_payload_bw_number = 0;
	else
		rep->u.path_resources.avail_payload_bw_number = full_pbn - dprx->payload_pbn_total;
}

static void dprx_execute_allocate_payload(struct dprx *dprx,
					  struct drm_dp_sideband_msg_req_body *req,
					  struct drm_dp_sideband_msg_reply_body *rep)
{
	struct drm_dp_allocate_payload *a_req = &req->u.allocate_payload;
	struct drm_dp_allocate_payload_ack_reply *a_rep = &rep->u.allocate_payload;
	int sink_idx;

	sink_idx = dprx_port_number_to_sink_idx(dprx, a_req->port_number);
	if (sink_idx == -1) {
		dprx_write_nak(dprx, rep, req->req_type, DP_NAK_BAD_PARAM);
		return;
	}

	if (a_req->vcpi == 0) {
		dprx_write_nak(dprx, rep, req->req_type, DP_NAK_BAD_PARAM);
		return;
	}

	if (a_req->pbn > 0) {
		if (dprx->payload_pbn[sink_idx] == 0) {
			/* New payload ID */
			dprx->payload_id[sink_idx] = a_req->vcpi;
		} else if (dprx->payload_id[sink_idx] != a_req->vcpi) {
			/* At most one payload ID is allowed per sink */
			dprx_write_nak(dprx, rep, req->req_type, DP_NAK_ALLOCATE_FAIL);
			return;
		}
	}
	WARN_ON_ONCE(dprx->payload_pbn_total < dprx->payload_pbn[sink_idx]);
	dprx->payload_pbn_total -= dprx->payload_pbn[sink_idx];
	dprx->payload_pbn_total += a_req->pbn;
	dprx->payload_pbn[sink_idx] = a_req->pbn;

	dprx_set_vc_ids(dprx);

	rep->reply_type = DP_SIDEBAND_REPLY_ACK;
	rep->req_type = DP_ALLOCATE_PAYLOAD;

	a_rep->port_number = a_req->port_number;
	a_rep->vcpi = a_req->vcpi;
	a_rep->allocated_pbn = a_req->pbn;
}

static void dprx_execute_clear_payload_id_table(struct dprx *dprx,
						struct drm_dp_sideband_msg_req_body *req,
						struct drm_dp_sideband_msg_reply_body *rep)
{
	rep->reply_type = DP_SIDEBAND_REPLY_ACK;
	rep->req_type = DP_CLEAR_PAYLOAD_ID_TABLE;

	dprx_clear_vc_payload_table(dprx);
}

static void dprx_execute_remote_dpcd_read(struct dprx *dprx,
					  struct drm_dp_sideband_msg_req_body *req,
					  struct drm_dp_sideband_msg_reply_body *rep)
{
	struct drm_dp_remote_dpcd_read *read_req = &req->u.dpcd_read;
	struct drm_dp_remote_dpcd_read_ack_reply *read_rep = &rep->u.remote_dpcd_read_ack;

	rep->reply_type = DP_SIDEBAND_REPLY_ACK;
	rep->req_type = DP_REMOTE_DPCD_READ;

	read_rep->port_number = read_req->port_number;
	read_rep->num_bytes = read_req->num_bytes;
	memset(read_rep->bytes, 0, read_req->num_bytes);
}

static void dprx_execute_remote_i2c_read(struct dprx *dprx,
					 struct drm_dp_sideband_msg_req_body *req,
					 struct drm_dp_sideband_msg_reply_body *rep)
{
	struct drm_dp_remote_i2c_read *read_req = &req->u.i2c_read;
	struct drm_dp_remote_i2c_read_ack_reply *read_rep = &rep->u.remote_i2c_read_ack;
	struct drm_dp_remote_i2c_read_tx *tx;
	struct dprx_sink *sink;
	int sink_idx;
	int res;
	int i;

	sink_idx = dprx_port_number_to_sink_idx(dprx, read_req->port_number);
	if (sink_idx == -1) {
		dprx_write_nak(dprx, rep, req->req_type, DP_NAK_BAD_PARAM);
		return;
	}
	sink = &dprx->sinks[sink_idx];

	for (i = 0; i < read_req->num_transactions; i++) {
		tx = &read_req->transactions[i];
		res = dprx_i2c_write(sink, tx->i2c_dev_id, tx->bytes, tx->num_bytes);
		if (res)
			goto i2c_err;
		if (!tx->no_stop_bit)
			dprx_i2c_stop(sink);
	}

	res = dprx_i2c_read(sink, read_req->read_i2c_device_id,
				  read_rep->bytes, read_req->num_bytes_read);
	if (res)
		goto i2c_err;
	dprx_i2c_stop(sink);

	rep->reply_type = DP_SIDEBAND_REPLY_ACK;
	rep->req_type = DP_REMOTE_I2C_READ;

	read_rep->port_number = read_req->port_number;
	read_rep->num_bytes = read_req->num_bytes_read;
	return;

i2c_err:
	dprx_i2c_stop(sink);
	dprx_write_nak(dprx, rep, req->req_type, DP_NAK_I2C_NAK);
}

static void dprx_execute_remote_i2c_write(struct dprx *dprx,
					  struct drm_dp_sideband_msg_req_body *req,
					  struct drm_dp_sideband_msg_reply_body *rep)
{
	struct drm_dp_remote_i2c_write *write_req = &req->u.i2c_write;
	struct drm_dp_remote_i2c_write_ack_reply *write_rep = &rep->u.remote_i2c_write_ack;
	struct dprx_sink *sink;
	int sink_idx;
	int res;

	sink_idx = dprx_port_number_to_sink_idx(dprx, write_req->port_number);
	if (sink_idx == -1) {
		dprx_write_nak(dprx, rep, req->req_type, DP_NAK_BAD_PARAM);
		return;
	}
	sink = &dprx->sinks[sink_idx];

	res = dprx_i2c_write(sink, write_req->write_i2c_device_id,
				   write_req->bytes, write_req->num_bytes);
	dprx_i2c_stop(sink);
	if (res) {
		dprx_write_nak(dprx, rep, req->req_type, DP_NAK_I2C_NAK);
		return;
	}

	rep->reply_type = DP_SIDEBAND_REPLY_ACK;
	rep->req_type = DP_REMOTE_I2C_WRITE;
	write_rep->port_number = write_req->port_number;
}

static void dprx_execute_power_up_phy(struct dprx *dprx,
				      struct drm_dp_sideband_msg_req_body *req,
				      struct drm_dp_sideband_msg_reply_body *rep)
{
	rep->reply_type = DP_SIDEBAND_REPLY_ACK;
	rep->req_type = DP_POWER_UP_PHY;
	rep->u.port_number.port_number = req->u.port_num.port_number;
}

static void dprx_execute_power_down_phy(struct dprx *dprx,
					struct drm_dp_sideband_msg_req_body *req,
					struct drm_dp_sideband_msg_reply_body *rep)
{
	rep->reply_type = DP_SIDEBAND_REPLY_ACK;
	rep->req_type = DP_POWER_DOWN_PHY;
	rep->u.port_number.port_number = req->u.port_num.port_number;
}

static void dprx_encode_sideband_msg(struct sideband_msg *msg, u8 *buf)
{
	int idx = 0;
	int i;
	u8 crc4;

	buf[idx++] = ((msg->lct & 0xf) << 4) | (msg->lcr & 0xf);
	for (i = 0; i < (msg->lct / 2); i++)
		buf[idx++] = msg->rad[i];
	buf[idx++] = (msg->broadcast << 7) | (msg->path_msg << 6) |
		((msg->body_len + 1) & 0x3f);
	buf[idx++] = (msg->somt << 7) | (msg->eomt << 6) | (msg->seqno << 4);

	crc4 = crc_dp_msg_header(buf, (idx * 2) - 1);
	buf[idx - 1] |= (crc4 & 0xf);

	memcpy(&buf[idx], msg->body, msg->body_len);
	idx += msg->body_len;
	buf[idx] = crc_dp_msg_data(msg->body, msg->body_len);
}

static bool dprx_decode_sideband_msg(struct sideband_msg *msg, u8 *buf, int buflen)
{
	u8 hdr_crc;
	u8 hdr_len;
	u8 body_crc;
	int i;
	u8 idx;

	if (buf[0] == 0)
		return false;
	hdr_len = 3;
	hdr_len += ((buf[0] & 0xf0) >> 4) / 2;
	if (hdr_len > buflen)
		return false;
	hdr_crc = crc_dp_msg_header(buf, (hdr_len * 2) - 1);
	if ((hdr_crc & 0xf) != (buf[hdr_len - 1] & 0xf))
		return false;

	msg->lct = (buf[0] & 0xf0) >> 4;
	msg->lcr = (buf[0] & 0xf);
	idx = 1;
	for (i = 0; i < (msg->lct / 2); i++)
		msg->rad[i] = buf[idx++];
	msg->broadcast = (buf[idx] >> 7) & 0x1;
	msg->path_msg = (buf[idx] >> 6) & 0x1;
	msg->body_len = (buf[idx] & 0x3f) - 1;
	idx++;
	msg->somt = (buf[idx] >> 7) & 0x1;
	msg->eomt = (buf[idx] >> 6) & 0x1;
	msg->seqno = (buf[idx] >> 4) & 0x1;
	idx++;

	if (hdr_len + msg->body_len + 1 != buflen)
		return false;

	body_crc = crc_dp_msg_data(&buf[idx], msg->body_len);
	if (body_crc != buf[idx + msg->body_len])
		return false;

	memcpy(msg->body, &buf[idx], msg->body_len);
	idx += msg->body_len;

	return true;
}

static bool dprx_decode_port_number_req(struct drm_dp_port_number_req *port_num, u8 *buf, int len)
{
	if (len != 1)
		return false;

	port_num->port_number = buf[0] >> 4;

	return true;
}

static bool
dprx_decode_connection_status_notify_req(struct drm_dp_connection_status_notify *conn_stat,
					 u8 *buf, int len)
{
	int idx = 0;

	if (len != 18)
		return false;

	conn_stat->port_number = buf[idx++];
	memcpy(conn_stat->guid, &buf[idx], 16);
	idx += 16;
	conn_stat->legacy_device_plug_status = (buf[idx] >> 6) & 1;
	conn_stat->displayport_device_plug_status = (buf[idx] >> 5) & 1;
	conn_stat->message_capability_status = (buf[idx] >> 4) & 1;
	conn_stat->input_port = (buf[idx] >> 3) & 1;
	conn_stat->peer_device_type = buf[idx] & 0x7;

	return true;
}

static bool dprx_decode_allocate_payload_req(struct drm_dp_allocate_payload *alloc_payload,
					     u8 *buf, int len)
{
	int idx = 0;
	int i;

	if (len < 4)
		return false;

	alloc_payload->port_number = buf[idx] >> 4;
	alloc_payload->number_sdp_streams = buf[idx++] & 0xf;
	alloc_payload->vcpi = buf[idx++] & 0x7f;
	alloc_payload->pbn = buf[idx] << 8 | buf[idx + 1];
	idx += 2;

	if (len != idx + (alloc_payload->number_sdp_streams + 1) / 2)
		return false;

	for (i = 0; i < alloc_payload->number_sdp_streams; i++) {
		if ((i & 1) == 0) {
			alloc_payload->sdp_stream_sink[i] = buf[idx] >> 4;
		} else {
			alloc_payload->sdp_stream_sink[i] = buf[idx] & 0xf;
			idx++;
		}
	}

	return true;
}

static bool dprx_decode_remote_dpcd_read_req(struct drm_dp_remote_dpcd_read *dpcd_read,
					     u8 *buf, int len)
{
	if (len != 4)
		return false;

	dpcd_read->port_number = buf[0] >> 4;
	dpcd_read->dpcd_address = (buf[0] & 0xf) << 16 | buf[1] << 8 | buf[2];
	dpcd_read->num_bytes = buf[3];

	return true;
}

static bool dprx_decode_remote_i2c_read_req(struct drm_dp_remote_i2c_read *i2c_read,
					    u8 *buf, int len)
{
	struct drm_dp_remote_i2c_read_tx *tx;
	int idx = 0;
	int i;

	if (len < 1)
		return false;

	i2c_read->port_number = buf[idx] >> 4;
	i2c_read->num_transactions = buf[idx] & 0x3;
	idx++;

	for (i = 0; i < i2c_read->num_transactions; i++) {
		tx = &i2c_read->transactions[i];
		if (len < idx + 2)
			return false;
		tx->i2c_dev_id = buf[idx++] & 0x7f;
		tx->num_bytes = buf[idx++];
		if (len < idx + tx->num_bytes + 1)
			return -1;
		tx->bytes = &buf[idx];
		idx += tx->num_bytes;
		tx->no_stop_bit = (buf[idx] >> 4) & 1;
		tx->i2c_transaction_delay = buf[idx] & 0xf;
		idx++;
	}

	if (len != idx + 2)
		return false;

	i2c_read->read_i2c_device_id = buf[idx++] & 0x7f;
	i2c_read->num_bytes_read = buf[idx++];

	return true;
}

static bool dprx_decode_remote_i2c_write_req(struct drm_dp_remote_i2c_write *i2c_write,
					     u8 *buf, int len)
{
	int idx = 0;

	if (len < 3)
		return false;

	i2c_write->port_number = buf[idx++] >> 4;
	i2c_write->write_i2c_device_id = buf[idx++] & 0x7f;
	i2c_write->num_bytes = buf[idx++];

	if (len != idx + i2c_write->num_bytes)
		return false;

	i2c_write->bytes = &buf[idx];

	return true;
}

static bool dprx_decode_sideband_req(struct drm_dp_sideband_msg_req_body *req, u8 *buf, int len)
{
	if (len == 0)
		return false;

	req->req_type = buf[0] & 0x7f;
	buf++;
	len--;

	switch (req->req_type) {
	case DP_LINK_ADDRESS:
	case DP_CLEAR_PAYLOAD_ID_TABLE:
		return len == 0; /* no request data */
	case DP_ENUM_PATH_RESOURCES:
	case DP_POWER_UP_PHY:
	case DP_POWER_DOWN_PHY:
		return dprx_decode_port_number_req(&req->u.port_num, buf, len);
	case DP_CONNECTION_STATUS_NOTIFY:
		return dprx_decode_connection_status_notify_req(&req->u.conn_stat, buf, len);
	case DP_ALLOCATE_PAYLOAD:
		return dprx_decode_allocate_payload_req(&req->u.allocate_payload, buf, len);
	case DP_REMOTE_DPCD_READ:
		return dprx_decode_remote_dpcd_read_req(&req->u.dpcd_read, buf, len);
	case DP_REMOTE_I2C_READ:
		return dprx_decode_remote_i2c_read_req(&req->u.i2c_read, buf, len);
	case DP_REMOTE_I2C_WRITE:
		return dprx_decode_remote_i2c_write_req(&req->u.i2c_write, buf, len);
	default:
		return false;
	}
}

static void dprx_encode_sideband_rep(struct drm_dp_sideband_msg_reply_body *rep, u8 *buf, int *len)
{
	int idx = 0;
	int i;

	buf[idx++] = (rep->reply_type & 0x1) << 7 | (rep->req_type & 0x7f);

	if (rep->reply_type) {
		memcpy(&buf[idx], rep->u.nak.guid, 16);
		idx += 16;
		buf[idx++] = rep->u.nak.reason;
		buf[idx++] = rep->u.nak.nak_data;
		*len = idx;
		return;
	}

	switch (rep->req_type) {
	case DP_LINK_ADDRESS: {
		struct drm_dp_link_address_ack_reply *link_addr = &rep->u.link_addr;
		struct drm_dp_link_addr_reply_port *port;

		memcpy(&buf[idx], link_addr->guid, 16);
		idx += 16;
		buf[idx++] = link_addr->nports;
		for (i = 0; i < link_addr->nports; i++) {
			port = &link_addr->ports[i];
			buf[idx++] = port->input_port << 7 | port->peer_device_type << 4 |
				     port->port_number;
			if (port->input_port == 0) {
				buf[idx++] = port->mcs << 7 | port->ddps << 6 |
					     port->legacy_device_plug_status << 5;
				buf[idx++] = port->dpcd_revision;
				memcpy(&buf[idx], port->peer_guid, 16);
				idx += 16;
				buf[idx++] = port->num_sdp_streams << 4 |
					     port->num_sdp_stream_sinks;
			} else {
				buf[idx++] = port->mcs << 7 | port->ddps << 6;
			}
		}
		break;
	}
	case DP_ENUM_PATH_RESOURCES: {
		struct drm_dp_enum_path_resources_ack_reply *path_res = &rep->u.path_resources;

		buf[idx++] = path_res->port_number << 4 | path_res->fec_capable;
		buf[idx++] = path_res->full_payload_bw_number >> 8;
		buf[idx++] = path_res->full_payload_bw_number & 0xff;
		buf[idx++] = path_res->avail_payload_bw_number >> 8;
		buf[idx++] = path_res->avail_payload_bw_number & 0xff;
		break;
	}
	case DP_ALLOCATE_PAYLOAD: {
		struct drm_dp_allocate_payload_ack_reply *alloc_payload = &rep->u.allocate_payload;

		buf[idx++] = alloc_payload->port_number << 4;
		buf[idx++] = alloc_payload->vcpi & 0x3f;
		buf[idx++] = alloc_payload->allocated_pbn >> 8;
		buf[idx++] = alloc_payload->allocated_pbn & 0xff;
		break;
	}
	case DP_REMOTE_DPCD_READ: {
		struct drm_dp_remote_dpcd_read_ack_reply *dpcd_read = &rep->u.remote_dpcd_read_ack;

		buf[idx++] = dpcd_read->port_number & 0xf;
		buf[idx++] = dpcd_read->num_bytes;
		memcpy(&buf[idx], dpcd_read->bytes, dpcd_read->num_bytes);
		idx += dpcd_read->num_bytes;
		break;
	}
	case DP_REMOTE_I2C_READ: {
		struct drm_dp_remote_i2c_read_ack_reply *i2c_read = &rep->u.remote_i2c_read_ack;

		buf[idx++] = i2c_read->port_number & 0xf;
		buf[idx++] = i2c_read->num_bytes;
		memcpy(&buf[idx], i2c_read->bytes, i2c_read->num_bytes);
		idx += i2c_read->num_bytes;
		break;
	}
	case DP_REMOTE_I2C_WRITE:
		buf[idx++] = rep->u.remote_i2c_write_ack.port_number & 0xf;
		break;
	case DP_POWER_UP_PHY:
	case DP_POWER_DOWN_PHY:
		buf[idx++] = rep->u.port_number.port_number << 4;
		break;
	}
	*len = idx;
}

static void dprx_execute_msg_transaction(struct dprx *dprx,
					 struct drm_dp_sideband_msg_req_body *req,
					 struct drm_dp_sideband_msg_reply_body *rep)
{
	switch (req->req_type) {
	case DP_LINK_ADDRESS:
		dprx_execute_link_address(dprx, req, rep);
		break;
	case DP_CONNECTION_STATUS_NOTIFY:
		dprx_execute_connection_status_notify(dprx, req, rep);
		break;
	case DP_ENUM_PATH_RESOURCES:
		dprx_execute_enum_path_resources(dprx, req, rep);
		break;
	case DP_ALLOCATE_PAYLOAD:
		dprx_execute_allocate_payload(dprx, req, rep);
		break;
	case DP_CLEAR_PAYLOAD_ID_TABLE:
		dprx_execute_clear_payload_id_table(dprx, req, rep);
		break;
	case DP_REMOTE_DPCD_READ:
		dprx_execute_remote_dpcd_read(dprx, req, rep);
		break;
	case DP_REMOTE_I2C_READ:
		dprx_execute_remote_i2c_read(dprx, req, rep);
		break;
	case DP_REMOTE_I2C_WRITE:
		dprx_execute_remote_i2c_write(dprx, req, rep);
		break;
	case DP_POWER_UP_PHY:
		dprx_execute_power_up_phy(dprx, req, rep);
		break;
	case DP_POWER_DOWN_PHY:
		dprx_execute_power_down_phy(dprx, req, rep);
		break;
	default:
		dprx_write_nak(dprx, rep, req->req_type, DP_NAK_BAD_PARAM);
		break;
	}
}

static void dprx_handle_msg_transaction(struct dprx *dprx,
					struct msg_transaction_rxbuf *rxbuf,
					struct msg_transaction_txbuf *txbuf)
{
	bool decoded;
	struct drm_dp_sideband_msg_req_body req;
	struct drm_dp_sideband_msg_reply_body rep;

	decoded = dprx_decode_sideband_req(&req, rxbuf->buf, rxbuf->len);
	if (decoded)
		dprx_execute_msg_transaction(dprx, &req, &rep);
	else
		dprx_write_nak(dprx, &rep, req.req_type, DP_NAK_BAD_PARAM);
	dprx_encode_sideband_rep(&rep, txbuf->buf, &txbuf->len);
	txbuf->written = 0;
}

static void dprx_msg_transaction_append(struct msg_transaction_rxbuf *rxbuf,
					struct msg_transaction_meta *meta,
					struct sideband_msg *msg)
{
	int append_len;

	append_len = min(msg->body_len, sizeof(rxbuf->buf) - rxbuf->len);
	memcpy(rxbuf->buf + rxbuf->len, msg->body, append_len);
	rxbuf->len += append_len;

	if (msg->somt) {
		meta->lct = msg->lct;
		memcpy(meta->rad, msg->rad, msg->lct / 2);
		meta->seqno = msg->seqno;
	}
}

static void dprx_msg_transaction_extract(struct msg_transaction_txbuf *txbuf,
					 struct msg_transaction_meta *meta,
					 struct sideband_msg *msg)
{
	int hdr_len = 3 + meta->lct / 2;
	int body_len;
	bool somt;
	bool eomt;

	body_len = txbuf->len - txbuf->written;
	/* trim body_len so that the sideband msg fits into 48 bytes */
	body_len = min(body_len, 48 - 1 - hdr_len);

	somt = (txbuf->written == 0);
	eomt = (txbuf->written + body_len == txbuf->len);

	msg->lct = meta->lct;
	msg->lcr = meta->lct - 1;
	memcpy(msg->rad, meta->rad, meta->lct / 2);
	msg->broadcast = false;
	msg->path_msg = false;
	msg->somt = somt;
	msg->eomt = eomt;
	msg->seqno = meta->seqno;

	memcpy(msg->body, txbuf->buf + txbuf->written, body_len);
	msg->body_len = body_len;

	txbuf->written += body_len;
}

static void dprx_msg_transaction_clear_rxbuf(struct msg_transaction_rxbuf *rxbuf)
{
	rxbuf->len = 0;
}

static void dprx_msg_transaction_clear_txbuf(struct msg_transaction_txbuf *txbuf)
{
	txbuf->len = 0;
	txbuf->written = 0;
}

static bool dprx_msg_transaction_txbuf_empty(struct msg_transaction_txbuf *txbuf)
{
	return txbuf->written == txbuf->len;
}

static void dprx_write_pending_sideband_msg(struct dprx *dprx)
{
	struct msg_transaction_txbuf *txbuf;
	struct msg_transaction_meta *meta;
	struct sideband_msg msg;

	if (WARN_ON_ONCE(!dprx->mt_pending))
		return;

	txbuf = &dprx->mt_txbuf[dprx->mt_seqno];
	meta = &dprx->mt_meta[dprx->mt_seqno];

	dprx_msg_transaction_extract(txbuf, meta, &msg);
	if (dprx_msg_transaction_txbuf_empty(txbuf)) {
		dprx->mt_seqno = !dprx->mt_seqno;
		txbuf = &dprx->mt_txbuf[dprx->mt_seqno];
		if (dprx_msg_transaction_txbuf_empty(txbuf))
			dprx->mt_pending = false;
	}

	dprx_encode_sideband_msg(&msg, dprx->down_rep_buf);
}

static void dprx_signal_irq(struct dprx *dprx, int irq)
{
	dprx->irq_vector |= irq;
	dprx_pulse_hpd(dprx);
}

static void dprx_handle_sideband_msg(struct dprx *dprx, struct sideband_msg *msg)
{
	struct msg_transaction_rxbuf *rxbuf = &dprx->mt_rxbuf[msg->seqno];
	struct msg_transaction_txbuf *txbuf = &dprx->mt_txbuf[msg->seqno];
	struct msg_transaction_meta *meta = &dprx->mt_meta[msg->seqno];

	if (msg->somt)
		dprx_msg_transaction_clear_rxbuf(rxbuf);
	dprx_msg_transaction_append(rxbuf, meta, msg);

	if (msg->eomt) {
		/* drop the message if txbuf isn't empty */
		if (!dprx_msg_transaction_txbuf_empty(txbuf))
			return;
		dprx_handle_msg_transaction(dprx, rxbuf, txbuf);

		if (!dprx->mt_pending) {
			dprx->mt_pending = true;
			dprx->mt_seqno = msg->seqno;
			if (!dprx->down_rep_pending) {
				dprx_write_pending_sideband_msg(dprx);
				dprx_signal_irq(dprx, DP_DOWN_REP_MSG_RDY);
				dprx->down_rep_pending = true;
			}
		}
	}
}

static void dprx_init_caps(struct dprx *dprx)
{
	memset(dprx->caps, 0, sizeof(dprx->caps));
	dprx->caps[DP_DPCD_REV]			= DP_DPCD_REV_14;
	dprx->caps[DP_MAX_LINK_RATE]		= dprx->max_link_rate;
	dprx->caps[DP_MAX_LANE_COUNT]		= DP_ENHANCED_FRAME_CAP | DP_TPS3_SUPPORTED |
						  dprx->max_lane_count;
	dprx->caps[DP_MAX_DOWNSPREAD]		= DP_TPS4_SUPPORTED | DP_MAX_DOWNSPREAD_0_5;
	dprx->caps[DP_MAIN_LINK_CHANNEL_CODING]	= DP_CAP_ANSI_8B10B;
	dprx->caps[DP_RECEIVE_PORT_0_CAP_0]	= DP_LOCAL_EDID_PRESENT;
}

static u8 dprx_read_caps(struct dprx *dprx, u32 offset)
{
	return dprx->caps[offset];
}

static u8 dprx_read_mstm_cap(struct dprx *dprx)
{
	return dprx->multi_stream_support;
}

static u8 dprx_read_guid(struct dprx *dprx, u32 offset)
{
	return dprx->guid[offset];
}

static void dprx_write_guid(struct dprx *dprx, u32 offset, u8 val)
{
	dprx->guid[offset] = val;
}

static u8 dprx_read_link_bw(struct dprx *dprx)
{
	u32 reg = dprx_read(dprx, DPRX_RX_CONTROL);

	return (reg >> DPRX_RX_CONTROL_LINK_RATE_SHIFT) & DPRX_RX_CONTROL_LINK_RATE_MASK;
}

static void dprx_write_link_bw(struct dprx *dprx, u8 val)
{
	u32 reg;

	if (val != DP_LINK_BW_1_62 && val != DP_LINK_BW_2_7 &&
	    val != DP_LINK_BW_5_4  && val != DP_LINK_BW_8_1)
		return;

	if (val > dprx->max_link_rate)
		return;

	reg = dprx_read(dprx, DPRX_RX_CONTROL);
	reg &= ~(DPRX_RX_CONTROL_LINK_RATE_MASK << DPRX_RX_CONTROL_LINK_RATE_SHIFT);
	reg |= val << DPRX_RX_CONTROL_LINK_RATE_SHIFT;
	reg |= 1 << DPRX_RX_CONTROL_RECONFIG_LINKRATE;
	dprx_write(dprx, DPRX_RX_CONTROL, reg);
}

static u8 dprx_read_lane_count(struct dprx *dprx)
{
	u32 reg = dprx_read(dprx, DPRX_RX_CONTROL);

	return (reg >> DPRX_RX_CONTROL_LANE_COUNT_SHIFT) & DPRX_RX_CONTROL_LANE_COUNT_MASK;
}

static void dprx_write_lane_count(struct dprx *dprx, u8 val)
{
	u32 reg;
	u8 lane_count;

	lane_count = val & DP_LANE_COUNT_MASK;

	if (lane_count != 1 && lane_count != 2 && lane_count != 4)
		return;

	if (lane_count > dprx->max_lane_count)
		return;

	reg = dprx_read(dprx, DPRX_RX_CONTROL);
	reg &= ~(DPRX_RX_CONTROL_LANE_COUNT_MASK << DPRX_RX_CONTROL_LANE_COUNT_SHIFT);
	reg |= lane_count << DPRX_RX_CONTROL_LANE_COUNT_SHIFT;
	dprx_write(dprx, DPRX_RX_CONTROL, reg);
}

static u8 dprx_read_training_pattern(struct dprx *dprx)
{
	u32 reg;
	u32 pattern;
	u32 scrambler_disable;
	u8 result = 0;

	reg = dprx_read(dprx, DPRX_RX_CONTROL);
	pattern = (reg >> DPRX_RX_CONTROL_TP_SHIFT) & DPRX_RX_CONTROL_TP_MASK;
	scrambler_disable = (reg >> DPRX_RX_CONTROL_SCRAMBLER_DISABLE) & 1;

	if (scrambler_disable)
		result |= DP_LINK_SCRAMBLING_DISABLE;
	result |= pattern;

	return result;
}

static void dprx_write_training_pattern(struct dprx *dprx, u8 val)
{
	u8 pattern;
	u8 scrambler_disable;
	u32 reg;

	pattern = val & DP_TRAINING_PATTERN_MASK_1_4;
	scrambler_disable = !!(val & DP_LINK_SCRAMBLING_DISABLE);

	reg = dprx_read(dprx, DPRX_RX_CONTROL);
	reg &= ~(DPRX_RX_CONTROL_TP_MASK << DPRX_RX_CONTROL_TP_SHIFT);
	reg |= pattern << DPRX_RX_CONTROL_TP_SHIFT;
	reg &= ~(1 << DPRX_RX_CONTROL_SCRAMBLER_DISABLE);
	reg |= scrambler_disable << DPRX_RX_CONTROL_SCRAMBLER_DISABLE;
	dprx_write(dprx, DPRX_RX_CONTROL, reg);
}

static u8 dprx_read_training_lane(struct dprx *dprx, u32 offset)
{
	struct dprx_training_control *ctl = &dprx->training_control[offset];
	u8 result = 0;

	result |= ctl->volt_swing << DP_TRAIN_VOLTAGE_SWING_SHIFT;
	if (ctl->max_swing)
		result |= DP_TRAIN_MAX_SWING_REACHED;
	result |= ctl->pre_emph << DP_TRAIN_PRE_EMPHASIS_SHIFT;
	if (ctl->max_pre_emph)
		result |= DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;

	return result;
}

static void dprx_write_training_lane(struct dprx *dprx, u32 offset, u8 val)
{
	struct dprx_training_control *ctl = &dprx->training_control[offset];

	ctl->volt_swing   = (val & DP_TRAIN_VOLTAGE_SWING_MASK) >> DP_TRAIN_VOLTAGE_SWING_SHIFT;
	ctl->max_swing    = (val & DP_TRAIN_MAX_SWING_REACHED);
	ctl->pre_emph     = (val & DP_TRAIN_PRE_EMPHASIS_MASK) >> DP_TRAIN_PRE_EMPHASIS_SHIFT;
	ctl->max_pre_emph = (val & DP_TRAIN_MAX_PRE_EMPHASIS_REACHED);
}

static u8 dprx_read_mstm_ctrl(struct dprx *dprx)
{
	return (dprx_read(dprx, DPRX_MST_CONTROL1) >> DPRX_MST_CONTROL1_MST_EN) & 1;
}

static void dprx_write_mstm_ctrl(struct dprx *dprx, u8 val)
{
	u8 mst_en = !!(val & DP_MST_EN);
	u32 reg;

	reg = dprx_read(dprx, DPRX_MST_CONTROL1);
	reg &= ~(1 << DPRX_MST_CONTROL1_MST_EN);
	reg |= mst_en << DPRX_MST_CONTROL1_MST_EN;
	dprx_write(dprx, DPRX_MST_CONTROL1, reg);
}

static void dprx_handle_payload_allocate(struct dprx *dprx)
{
	u8 id = dprx->payload_allocate_set;
	u8 start = dprx->payload_allocate_start_time_slot;
	u8 count = dprx->payload_allocate_time_slot_count;

	if (id == 0 && start == 0 && count == 0x3f) {
		dprx_clear_vc_payload_table(dprx);
	} else {
		if (count == 0)
			dprx_deallocate_vc_payload(dprx, start, id);
		else
			dprx_allocate_vc_payload(dprx, start, count, id);
		dprx_set_vc_payload_table(dprx);
	}
	dprx->payload_table_updated = 1;
}

static u8 dprx_read_payload_allocate_set(struct dprx *dprx)
{
	return dprx->payload_allocate_set;
}

static void dprx_write_payload_allocate_set(struct dprx *dprx, u8 val)
{
	dprx->payload_allocate_set = val & DP_PAYLOAD_ALLOCATE_SET_MASK;
}

static u8 dprx_read_payload_allocate_start_time_slot(struct dprx *dprx)
{
	return dprx->payload_allocate_start_time_slot;
}

static void dprx_write_payload_allocate_start_time_slot(struct dprx *dprx, u8 val)
{
	dprx->payload_allocate_start_time_slot = val & DP_PAYLOAD_ALLOCATE_START_TIME_SLOT_MASK;
}

static u8 dprx_read_payload_allocate_time_slot_count(struct dprx *dprx)
{
	return dprx->payload_allocate_time_slot_count;
}

static void dprx_write_payload_allocate_time_slot_count(struct dprx *dprx, u8 val)
{
	dprx->payload_allocate_time_slot_count = val & DP_PAYLOAD_ALLOCATE_TIME_SLOT_COUNT_MASK;
	dprx_handle_payload_allocate(dprx);
}

static u8 dprx_read_sink_count(struct dprx *dprx)
{
	return dprx->max_stream_count;
}

static u8 dprx_read_device_service_irq_vector(struct dprx *dprx)
{
	return dprx->irq_vector;
}

static void dprx_write_device_service_irq_vector(struct dprx *dprx, u8 val)
{
	dprx->irq_vector &= ~val;

	if (val & DP_DOWN_REP_MSG_RDY) {
		if (dprx->mt_pending) {
			dprx_write_pending_sideband_msg(dprx);
			dprx_signal_irq(dprx, DP_DOWN_REP_MSG_RDY);
		} else {
			dprx->down_rep_pending = false;
		}
	}
}

static u8 dprx_read_lane0_1_status(struct dprx *dprx)
{
	u32 reg;
	u8 res = 0;

	reg = dprx_read(dprx, DPRX_RX_STATUS);
	if ((reg >> DPRX_RX_STATUS_CR_LOCK(0)) & 1)
		res |= DP_LANE_CR_DONE;
	if ((reg >> DPRX_RX_STATUS_CR_LOCK(1)) & 1)
		res |= DP_LANE_CR_DONE << 4;
	if ((reg >> DPRX_RX_STATUS_SYM_LOCK(0)) & 1)
		res |= DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED;
	if ((reg >> DPRX_RX_STATUS_SYM_LOCK(1)) & 1)
		res |= (DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED) << 4;

	return res;
}

static u8 dprx_read_lane2_3_status(struct dprx *dprx)
{
	u32 reg;
	u8 res = 0;

	reg = dprx_read(dprx, DPRX_RX_STATUS);
	if ((reg >> DPRX_RX_STATUS_CR_LOCK(2)) & 1)
		res |= DP_LANE_CR_DONE;
	if ((reg >> DPRX_RX_STATUS_CR_LOCK(3)) & 1)
		res |= DP_LANE_CR_DONE << 4;
	if ((reg >> DPRX_RX_STATUS_SYM_LOCK(2)) & 1)
		res |= DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED;
	if ((reg >> DPRX_RX_STATUS_SYM_LOCK(3)) & 1)
		res |= (DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED) << 4;

	return res;
}

static u8 dprx_read_lane_align_status(struct dprx *dprx)
{
	return (dprx_read(dprx, DPRX_RX_STATUS) >> DPRX_RX_STATUS_INTERLANE_ALIGN) & 1;
}

static u8 dprx_read_sink_status(struct dprx *dprx)
{
	return (dprx_read(dprx, DPRX_VBID(0)) >> DPRX_VBID_MSA_LOCK) & 1;
}

static u8 dprx_read_adjust_request(struct dprx *dprx,
				   struct dprx_training_control *ctl0,
				   struct dprx_training_control *ctl1)
{
	u8 next_volt_swing0;
	u8 next_pre_emph0;
	u8 next_volt_swing1;
	u8 next_pre_emph1;

	if (dprx_adjust_needed(dprx)) {
		dprx_training_control_next(ctl0, &next_volt_swing0, &next_pre_emph0);
		dprx_training_control_next(ctl1, &next_volt_swing1, &next_pre_emph1);
	} else {
		next_volt_swing0 = ctl0->volt_swing;
		next_pre_emph0 = ctl0->pre_emph;
		next_volt_swing1 = ctl1->volt_swing;
		next_pre_emph1 = ctl1->pre_emph;
	}

	return next_volt_swing0 << DP_ADJUST_VOLTAGE_SWING_LANE0_SHIFT |
	       next_pre_emph0   << DP_ADJUST_PRE_EMPHASIS_LANE0_SHIFT  |
	       next_volt_swing1 << DP_ADJUST_VOLTAGE_SWING_LANE1_SHIFT |
	       next_pre_emph1   << DP_ADJUST_PRE_EMPHASIS_LANE1_SHIFT;
}

static u8 dprx_read_adjust_request_lane0_1(struct dprx *dprx)
{
	return dprx_read_adjust_request(dprx,
					&dprx->training_control[0],
					&dprx->training_control[1]);
}

static u8 dprx_read_adjust_request_lane2_3(struct dprx *dprx)
{
	return dprx_read_adjust_request(dprx,
					&dprx->training_control[2],
					&dprx->training_control[3]);
}

static u8 dprx_read_payload_table_update_status(struct dprx *dprx)
{
	u32 reg;
	u32 act_handled;
	u8 result = 0;

	reg = dprx_read(dprx, DPRX_MST_STATUS1);
	act_handled = (reg >> DPRX_MST_STATUS1_VCPTAB_ACT_ACK) & 1;

	if (dprx->payload_table_updated)
		result |= DP_PAYLOAD_TABLE_UPDATED;
	if (act_handled)
		result |= DP_PAYLOAD_ACT_HANDLED;

	return result;
}

static void dprx_write_payload_table_update_status(struct dprx *dprx, u8 val)
{
	u32 reg;

	if (val & DP_PAYLOAD_TABLE_UPDATED) {
		dprx->payload_table_updated = 0;
		reg = dprx_read(dprx, DPRX_MST_CONTROL1);
		reg &= ~(1 << DPRX_MST_CONTROL1_VCPTAB_UPD_REQ);
		dprx_write(dprx, DPRX_MST_CONTROL1, reg);
	}
}

static u8 dprx_read_vc_payload_id_slot(struct dprx *dprx, u32 offset)
{
	return dprx->payload_table[offset + 1];
}

static u8 dprx_read_down_req(struct dprx *dprx, u32 offset)
{
	return dprx->down_req_buf[offset];
}

static void dprx_write_down_req(struct dprx *dprx, u32 offset, u8 val)
{
	struct sideband_msg msg;

	dprx->down_req_buf[offset] = val;
	if (dprx_decode_sideband_msg(&msg, dprx->down_req_buf, offset + 1))
		dprx_handle_sideband_msg(dprx, &msg);
}

static u8 dprx_read_down_rep(struct dprx *dprx, u32 offset)
{
	return dprx->down_rep_buf[offset];
}

struct dprx_dpcd_handler {
	u32 addr;
	u32 range_len;
	union {
		u8 (*point)(struct dprx *dprx);
		u8 (*range)(struct dprx *dprx, u32 offset);
	} read;
	union {
		void (*point)(struct dprx *dprx, u8 val);
		void (*range)(struct dprx *dprx, u32 offset, u8 val);
	} write;
};

static void dprx_write_noop(struct dprx *dprx, u8 val)
{
}

static void dprx_write_noop_range(struct dprx *dprx, u32 offset, u8 val)
{
}

static struct dprx_dpcd_handler dprx_dpcd_handlers[] = {
	{ 0x00000, 16,	{ .range = dprx_read_caps },
			{ .range = dprx_write_noop_range } },
	{ 0x00021, 0,	{ .point = dprx_read_mstm_cap },
			{ .point = dprx_write_noop } },
	{ 0x00030, 16,	{ .range = dprx_read_guid },
			{ .range = dprx_write_guid } },
	{ 0x00100, 0,	{ .point = dprx_read_link_bw },
			{ .point = dprx_write_link_bw } },
	{ 0x00101, 0,	{ .point = dprx_read_lane_count },
			{ .point = dprx_write_lane_count } },
	{ 0x00102, 0,	{ .point = dprx_read_training_pattern },
			{ .point = dprx_write_training_pattern } },
	{ 0x00103, 4,	{ .range = dprx_read_training_lane },
			{ .range = dprx_write_training_lane } },
	{ 0x00111, 0,	{ .point = dprx_read_mstm_ctrl },
			{ .point = dprx_write_mstm_ctrl } },
	{ 0x001c0, 0,	{ .point = dprx_read_payload_allocate_set },
			{ .point = dprx_write_payload_allocate_set } },
	{ 0x001c1, 0,	{ .point = dprx_read_payload_allocate_start_time_slot },
			{ .point = dprx_write_payload_allocate_start_time_slot } },
	{ 0x001c2, 0,	{ .point = dprx_read_payload_allocate_time_slot_count },
			{ .point = dprx_write_payload_allocate_time_slot_count } },
	{ 0x00200, 0,	{ .point = dprx_read_sink_count },
			{ .point = dprx_write_noop } },
	{ 0x00201, 0,	{ .point = dprx_read_device_service_irq_vector },
			{ .point = dprx_write_device_service_irq_vector } },
	{ 0x00202, 0,	{ .point = dprx_read_lane0_1_status },
			{ .point = dprx_write_noop } },
	{ 0x00203, 0,	{ .point = dprx_read_lane2_3_status },
			{ .point = dprx_write_noop } },
	{ 0x00204, 0,	{ .point = dprx_read_lane_align_status },
			{ .point = dprx_write_noop } },
	{ 0x00205, 0,	{ .point = dprx_read_sink_status },
			{ .point = dprx_write_noop } },
	{ 0x00206, 0,	{ .point = dprx_read_adjust_request_lane0_1 },
			{ .point = dprx_write_noop } },
	{ 0x00207, 0,	{ .point = dprx_read_adjust_request_lane2_3 },
			{ .point = dprx_write_noop } },
	{ 0x002c0, 0,	{ .point = dprx_read_payload_table_update_status },
			{ .point = dprx_write_payload_table_update_status } },
	{ 0x002c1, 63,	{ .range = dprx_read_vc_payload_id_slot },
			{ .range = dprx_write_noop_range } },
	{ 0x01000, 48,	{ .range = dprx_read_down_req },
			{ .range = dprx_write_down_req } },
	{ 0x01400, 48,	{ .range = dprx_read_down_rep },
			{ .range = dprx_write_noop_range } },
	/* Event Status Indicator is a copy of 200h - 205h */
	{ 0x02002, 0,	{ .point = dprx_read_sink_count },
			{ .point = dprx_write_noop } },
	{ 0x02003, 0,	{ .point = dprx_read_device_service_irq_vector },
			{ .point = dprx_write_device_service_irq_vector } },
	{ 0x0200c, 0,	{ .point = dprx_read_lane0_1_status },
			{ .point = dprx_write_noop } },
	{ 0x0200d, 0,	{ .point = dprx_read_lane2_3_status },
			{ .point = dprx_write_noop } },
	{ 0x0200e, 0,	{ .point = dprx_read_lane_align_status },
			{ .point = dprx_write_noop } },
	{ 0x0200f, 0,	{ .point = dprx_read_sink_status },
			{ .point = dprx_write_noop } },
	/* Extended Receiver Capability is a copy of 0h - 0fh */
	{ 0x02200, 16,	{ .range = dprx_read_caps },
			{ .range = dprx_write_noop_range } },
};

static bool dprx_dpcd_handler_match(struct dprx_dpcd_handler *handler, u32 addr)
{
	if (handler->range_len == 0)
		return addr == handler->addr;
	else
		return addr >= handler->addr && addr < handler->addr + handler->range_len;
}

static void dprx_dpcd_handler_run(struct dprx_dpcd_handler *handler,
				  struct dprx *dprx, u32 addr, u8 *val, bool read)
{
	if (read) {
		if (handler->range_len == 0)
			*val = handler->read.point(dprx);
		else
			*val = handler->read.range(dprx, addr - handler->addr);
	} else {
		if (handler->range_len == 0)
			handler->write.point(dprx, *val);
		else
			handler->write.range(dprx, addr - handler->addr, *val);
	}
}

static void dprx_dpcd_access(struct dprx *dprx, u32 addr, u8 *val, bool read)
{
	struct dprx_dpcd_handler *handler;
	int i;

	for (i = 0; i < ARRAY_SIZE(dprx_dpcd_handlers); i++) {
		handler = &dprx_dpcd_handlers[i];
		if (dprx_dpcd_handler_match(handler, addr)) {
			dprx_dpcd_handler_run(handler, dprx, addr, val, read);
			return;
		}
	}

	/* for unsupported registers, writes are ignored and reads return 0. */
	if (read)
		*val = 0;
}

static void dprx_handle_native_aux(struct dprx *dprx, struct aux_msg *req, struct aux_msg *rep)
{
	bool read = req->cmd & 1;
	u8 *data;
	int i;

	rep->cmd = DP_AUX_NATIVE_REPLY_ACK;
	if (read) {
		rep->len = req->len;
		data = rep->data;
	} else {
		rep->len = 0;
		data = req->data;
	}

	for (i = 0; i < req->len; i++)
		dprx_dpcd_access(dprx, req->addr + i, data + i, read);
}

static void dprx_handle_i2c_read(struct dprx *dprx, struct aux_msg *req, struct aux_msg *rep)
{
	int res;

	res = dprx_i2c_read(&dprx->sinks[0], req->addr, rep->data, req->len);
	if (!res) {
		rep->cmd = DP_AUX_I2C_REPLY_ACK;
		rep->len = req->len;
	} else {
		rep->cmd = DP_AUX_I2C_REPLY_NACK;
		rep->len = 0;
	}
}

static void dprx_handle_i2c_write(struct dprx *dprx, struct aux_msg *req, struct aux_msg *rep)
{
	int res;

	res = dprx_i2c_write(&dprx->sinks[0], req->addr, req->data, req->len);
	if (!res)
		rep->cmd = DP_AUX_I2C_REPLY_ACK;
	else
		rep->cmd = DP_AUX_I2C_REPLY_NACK;
	rep->len = 0;
}

static void dprx_decode_aux_request(struct aux_msg *req, struct aux_buf *buf)
{
	req->cmd = buf->data[0] >> 4;
	req->addr = (buf->data[0] & 0xf) << 16 | buf->data[1] << 8 | buf->data[2];
	if (buf->len < 4) {
		req->len = 0;
	} else {
		req->len = buf->data[3] + 1;
		memcpy(req->data, &buf->data[4], req->len);
	}
}

static void dprx_encode_aux_reply(struct aux_msg *rep, struct aux_buf *buf)
{
	buf->data[0] = rep->cmd << 4;
	memcpy(&buf->data[1], rep->data, rep->len);
	buf->len = rep->len + 1;
}

static void dprx_handle_aux(struct dprx *dprx, struct aux_buf *req_buf, struct aux_buf *rep_buf)
{
	struct aux_msg req;
	struct aux_msg rep;

	dprx_decode_aux_request(&req, req_buf);

	if (req.cmd & 8) {
		dprx_handle_native_aux(dprx, &req, &rep);
	} else {
		if (req.cmd & 1)
			dprx_handle_i2c_read(dprx, &req, &rep);
		else
			dprx_handle_i2c_write(dprx, &req, &rep);
		if (!(req.cmd & DP_AUX_I2C_MOT))
			dprx_i2c_stop(&dprx->sinks[0]);
	}

	dprx_encode_aux_reply(&rep, rep_buf);
}

static int dprx_read_aux(struct dprx *dprx, struct aux_buf *buf)
{
	u32 control = dprx_read(dprx, DPRX_AUX_CONTROL);
	int i;

	/* check MSG_READY */
	if (!((dprx_read(dprx, DPRX_AUX_STATUS) >> DPRX_AUX_STATUS_MSG_READY) & 1))
		return -1;

	/* read LENGTH */
	buf->len = (control >> DPRX_AUX_CONTROL_LENGTH_SHIFT) & DPRX_AUX_CONTROL_LENGTH_MASK;
	if (buf->len > 20)
		buf->len = 20;

	/* read request */
	for (i = 0; i < buf->len; i++)
		buf->data[i] = dprx_read(dprx, DPRX_AUX_COMMAND + i);

	return 0;
}

static void dprx_write_aux(struct dprx *dprx, struct aux_buf *buf)
{
	u32 reg;
	int i;

	if (!((dprx_read(dprx, DPRX_AUX_STATUS) >> DPRX_AUX_STATUS_READY_TO_TX) & 1))
		return;

	if (buf->len > 17)
		buf->len = 17;
	for (i = 0; i < buf->len; i++)
		dprx_write(dprx, DPRX_AUX_COMMAND + i, buf->data[i]);

	reg = dprx_read(dprx, DPRX_AUX_CONTROL);
	reg &= ~(DPRX_AUX_CONTROL_LENGTH_MASK << DPRX_AUX_CONTROL_LENGTH_SHIFT);
	reg |= buf->len << DPRX_AUX_CONTROL_LENGTH_SHIFT;
	reg |= 1 << DPRX_AUX_CONTROL_TX_STROBE;
	dprx_write(dprx, DPRX_AUX_CONTROL, reg);
}

static irqreturn_t dprx_isr(int irq, void *data)
{
	struct dprx *dprx = data;
	struct aux_buf request;
	struct aux_buf reply;

	if (!dprx_read_aux(dprx, &request)) {
		spin_lock(&dprx->lock);
		dprx_handle_aux(dprx, &request, &reply);
		spin_unlock(&dprx->lock);
		dprx_write_aux(dprx, &reply);
	}

	return IRQ_HANDLED;
}

static void dprx_reset_hw(struct dprx *dprx)
{
	int i;

	/* set link rate to 1.62 Gbps and lane count to 1 */
	dprx_write(dprx, DPRX_RX_CONTROL,
		   DP_LINK_BW_1_62 << DPRX_RX_CONTROL_LINK_RATE_SHIFT |
		   1 << DPRX_RX_CONTROL_RECONFIG_LINKRATE |
		   DPRX_RX_CONTROL_CHANNEL_CODING_8B10B << DPRX_RX_CONTROL_CHANNEL_CODING_SHIFT |
		   1 << DPRX_RX_CONTROL_LANE_COUNT_SHIFT);
	/* clear VC payload ID table */
	for (i = 0; i < 8; i++)
		dprx_write(dprx, DPRX_MST_VCPTAB(i), 0);
	dprx_write(dprx, DPRX_MST_CONTROL1, 1 << DPRX_MST_CONTROL1_VCPTAB_UPD_FORCE);
}

static void dprx_reset(struct dprx *dprx)
{
	int i;

	memset(dprx->guid, 0, sizeof(dprx->guid));
	memset(dprx->training_control, 0, sizeof(dprx->training_control));

	dprx->payload_allocate_set = 0;
	dprx->payload_allocate_start_time_slot = 0;
	dprx->payload_allocate_time_slot_count = 0;
	memset(dprx->payload_table, 0, sizeof(dprx->payload_table));
	dprx->payload_table_updated = 0;

	memset(dprx->payload_id, 0, sizeof(dprx->payload_id));
	memset(dprx->payload_pbn, 0, sizeof(dprx->payload_pbn));
	dprx->payload_pbn_total = 0;

	dprx->irq_vector = 0;

	memset(dprx->down_req_buf, 0, sizeof(dprx->down_req_buf));
	memset(dprx->down_rep_buf, 0, sizeof(dprx->down_rep_buf));

	for (i = 0; i < 2; i++) {
		dprx_msg_transaction_clear_rxbuf(&dprx->mt_rxbuf[i]);
		dprx_msg_transaction_clear_txbuf(&dprx->mt_txbuf[i]);
	}
	dprx->mt_seqno = 0;
	dprx->mt_pending = false;
	dprx->down_rep_pending = false;

	dprx_reset_hw(dprx);
}

#define to_dprx(sd) container_of(sd, struct dprx, subdev)

static int dprx_get_edid(struct v4l2_subdev *sd, struct v4l2_edid *edid)
{
	struct dprx *dprx = to_dprx(sd);
	struct dprx_sink *sink;
	u32 end_block = edid->start_block + edid->blocks;
	unsigned long flags;
	int res = 0;

	memset(edid->reserved, 0, sizeof(edid->reserved));

	if (edid->pad >= dprx->max_stream_count)
		return -EINVAL;

	spin_lock_irqsave(&dprx->lock, flags);

	sink = &dprx->sinks[edid->pad];
	if (edid->start_block == 0 && edid->blocks == 0) {
		edid->blocks = sink->blocks;
		goto out;
	}
	if (sink->blocks == 0) {
		res = -ENODATA;
		goto out;
	}
	if (edid->start_block >= sink->blocks) {
		res = -EINVAL;
		goto out;
	}
	if (end_block > sink->blocks) {
		end_block = sink->blocks;
		edid->blocks = end_block - edid->start_block;
	}

	memcpy(edid->edid, sink->edid + edid->start_block * 128, edid->blocks * 128);

out:
	spin_unlock_irqrestore(&dprx->lock, flags);

	return res;
}

static int dprx_set_edid(struct v4l2_subdev *sd, struct v4l2_edid *edid)
{
	struct dprx *dprx = to_dprx(sd);
	struct dprx_sink *sink;
	bool prev_hpd;
	bool cur_hpd;
	unsigned long flags;

	memset(edid->reserved, 0, sizeof(edid->reserved));

	if (edid->pad >= dprx->max_stream_count)
		return -EINVAL;
	if (edid->start_block != 0)
		return -EINVAL;
	if (edid->blocks > DPRX_MAX_EDID_BLOCKS) {
		edid->blocks = DPRX_MAX_EDID_BLOCKS;
		return -E2BIG;
	}

	spin_lock_irqsave(&dprx->lock, flags);
	sink = &dprx->sinks[edid->pad];
	/*
	 * This is an MST DisplayPort device, which means that one HPD
	 * line controls all the video streams. The way this is handled
	 * in s_edid is that the HPD line is controlled by the presence
	 * of only the first stream's EDID. This allows, for example, to
	 * first set the second streams's EDID and then the first one in
	 * order to reduce the amount of AUX communication.
	 */
	prev_hpd = dprx->sinks[0].blocks > 0;
	sink->blocks = edid->blocks;
	memcpy(sink->edid, edid->edid, edid->blocks * 128);
	cur_hpd = dprx->sinks[0].blocks > 0;
	if (!prev_hpd && cur_hpd)
		dprx_reset(dprx);
	spin_unlock_irqrestore(&dprx->lock, flags);

	if (!prev_hpd && cur_hpd) {
		dprx_set_hpd(dprx, 1);
	} else if (prev_hpd && !cur_hpd) {
		dprx_set_hpd(dprx, 0);
	} else if (prev_hpd && cur_hpd) {
		/* HPD replug - pulse for >2ms */
		dprx_set_hpd(dprx, 0);
		usleep_range(2000, 4000);
		dprx_set_hpd(dprx, 1);
	}

	return 0;
}

static int dprx_query_dv_timings(struct v4l2_subdev *sd, unsigned int pad,
				 struct v4l2_dv_timings *timings)
{
	struct dprx *dprx = to_dprx(sd);
	u32 htotal, vtotal;
	u32 hsp, hsw;
	u32 hstart, vstart;
	u32 vsp, vsw;
	u32 hwidth, vheight;

	if (pad >= dprx->max_stream_count)
		return -EINVAL;

	if (!((dprx_read(dprx, DPRX_VBID(pad)) >> DPRX_VBID_MSA_LOCK) & 1))
		return -ENOLINK;

	htotal  = dprx_read(dprx, DPRX_MSA_HTOTAL(pad));
	vtotal  = dprx_read(dprx, DPRX_MSA_VTOTAL(pad));
	hsp     = dprx_read(dprx, DPRX_MSA_HSP(pad));
	hsw     = dprx_read(dprx, DPRX_MSA_HSW(pad));
	hstart  = dprx_read(dprx, DPRX_MSA_HSTART(pad));
	vstart  = dprx_read(dprx, DPRX_MSA_VSTART(pad));
	vsp     = dprx_read(dprx, DPRX_MSA_VSP(pad));
	vsw     = dprx_read(dprx, DPRX_MSA_VSW(pad));
	hwidth  = dprx_read(dprx, DPRX_MSA_HWIDTH(pad));
	vheight = dprx_read(dprx, DPRX_MSA_VHEIGHT(pad));

	memset(timings, 0, sizeof(*timings));
	timings->type = V4L2_DV_BT_656_1120;
	timings->bt.width = hwidth;
	timings->bt.height = vheight;
	timings->bt.polarities = (!vsp) | (!hsp) << 1;
	timings->bt.hfrontporch = htotal - hstart - hwidth;
	timings->bt.hsync = hsw;
	timings->bt.hbackporch = hstart - hsw;
	timings->bt.vfrontporch = vtotal - vstart - vheight;
	timings->bt.vsync = vsw;
	timings->bt.vbackporch = vstart - vsw;

	return 0;
}

/* DisplayPort 1.4 capabilities */

static const struct v4l2_dv_timings_cap dprx_timings_cap = {
	.type = V4L2_DV_BT_656_1120,
	.bt = {
		.min_width = 0,
		.max_width = 7680,
		.min_height = 0,
		.max_height = 4320,
		.min_pixelclock = 0,
		.max_pixelclock = 1350000000, /* 8.1Gbps * 4lanes / 24bpp */
		.standards = V4L2_DV_BT_STD_CEA861 | V4L2_DV_BT_STD_DMT |
			V4L2_DV_BT_STD_CVT | V4L2_DV_BT_STD_GTF,
		.capabilities = V4L2_DV_BT_CAP_PROGRESSIVE |
			V4L2_DV_BT_CAP_REDUCED_BLANKING |
			V4L2_DV_BT_CAP_CUSTOM,
	},
};

static int dprx_enum_dv_timings(struct v4l2_subdev *sd, struct v4l2_enum_dv_timings *timings)
{
	return v4l2_enum_dv_timings_cap(timings, &dprx_timings_cap,
					NULL, NULL);
}

static int dprx_dv_timings_cap(struct v4l2_subdev *sd, struct v4l2_dv_timings_cap *cap)
{
	*cap = dprx_timings_cap;

	return 0;
}

static const struct v4l2_subdev_pad_ops dprx_pad_ops = {
	.get_edid = dprx_get_edid,
	.set_edid = dprx_set_edid,
	.dv_timings_cap = dprx_dv_timings_cap,
	.enum_dv_timings = dprx_enum_dv_timings,
	.query_dv_timings = dprx_query_dv_timings,
};

static const struct v4l2_subdev_ops dprx_subdev_ops = {
	.pad = &dprx_pad_ops,
};

static const struct media_entity_operations dprx_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.get_fwnode_pad = v4l2_subdev_get_fwnode_pad_1_to_1,
};

static int dprx_init_pads(struct dprx *dprx)
{
	int i;

	for (i = 0; i < dprx->max_stream_count; i++)
		dprx->pads[i].flags = MEDIA_PAD_FL_SOURCE;

	return media_entity_pads_init(&dprx->subdev.entity, dprx->max_stream_count, dprx->pads);
}

static int dprx_probe(struct platform_device *pdev)
{
	struct dprx *dprx;
	int irq;
	int res;

	dprx = devm_kzalloc(&pdev->dev, sizeof(*dprx), GFP_KERNEL);
	if (!dprx)
		return -ENOMEM;
	dprx->dev = &pdev->dev;
	platform_set_drvdata(pdev, dprx);

	dprx->iobase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dprx->iobase))
		return PTR_ERR(dprx->iobase);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	res = devm_request_irq(dprx->dev, irq, dprx_isr, 0, "intel-dprx", dprx);
	if (res)
		return res;

	res = device_property_read_u32(&pdev->dev, "intel,max-link-rate", &dprx->max_link_rate);
	if (res)
		return res;

	res = device_property_read_u32(&pdev->dev, "intel,max-lane-count", &dprx->max_lane_count);
	if (res)
		return res;

	dprx->multi_stream_support = device_property_read_bool(&pdev->dev,
							       "intel,multi-stream-support");

	if (dprx->multi_stream_support) {
		res = device_property_read_u32(&pdev->dev, "intel,max-stream-count",
					       &dprx->max_stream_count);
		if (res)
			return res;
	} else {
		dprx->max_stream_count = 1;
	}

	dprx_init_caps(dprx);

	dprx->subdev.owner = THIS_MODULE;
	dprx->subdev.dev = &pdev->dev;
	v4l2_subdev_init(&dprx->subdev, &dprx_subdev_ops);
	v4l2_set_subdevdata(&dprx->subdev, &pdev->dev);
	snprintf(dprx->subdev.name, sizeof(dprx->subdev.name), "%s %s",
		 KBUILD_MODNAME, dev_name(&pdev->dev));
	dprx->subdev.flags = V4L2_SUBDEV_FL_HAS_DEVNODE;

	dprx->subdev.entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;
	dprx->subdev.entity.ops = &dprx_entity_ops;

	res = dprx_init_pads(dprx);
	if (res)
		return res;

	res = v4l2_async_register_subdev(&dprx->subdev);
	if (res)
		return res;

	dprx_set_hpd(dprx, 0);
	dprx_reset_hw(dprx);

	dprx_set_irq(dprx, 1);

	return 0;
}

static void dprx_remove(struct platform_device *pdev)
{
	struct dprx *dprx = platform_get_drvdata(pdev);

	/* disable interrupts */
	dprx_set_irq(dprx, 0);

	v4l2_async_unregister_subdev(&dprx->subdev);
}

static const struct of_device_id dprx_match_table[] = {
	{ .compatible = "intel,dprx-20.0.1" },
	{ },
};

static struct platform_driver dprx_platform_driver = {
	.probe = dprx_probe,
	.remove_new = dprx_remove,
	.driver = {
		.name = "intel-dprx",
		.of_match_table = dprx_match_table,
	},
};

module_platform_driver(dprx_platform_driver);

MODULE_AUTHOR("Paweł Anikiel <panikiel@google.com>");
MODULE_DESCRIPTION("Intel DisplayPort RX IP core driver");
MODULE_LICENSE("GPL");
