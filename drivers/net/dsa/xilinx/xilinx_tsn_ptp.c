// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD/Xilinx TSN Endpoint Ethernet MAC DSA switch driver:
 * PTP hardware clock and per-MAC PTP TX/RX paths.
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/net_tstamp.h>
#include <linux/netdevice.h>
#include <linux/of_irq.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/time64.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/dsa.h>

#include "xilinx_tsn.h"

/* Reading the nanoseconds register latches a full RTC snapshot
 * (ns + sec_low + sec_high). The subsequent seconds reads return
 * those latched values, so nanoseconds must be read first.
 */
static void xlnx_tsn_tod_read(struct xlnx_tsn *sw, struct timespec64 *ts)
{
	struct xlnx_tsn_mac *m = &sw->mac[XLNX_TSN_PORT_MAC1];
	u32 secl, sech, nsec;

	nsec = mac_ior(m, TSN_TIMER_CURRENT_RTC_NS);
	secl = mac_ior(m, TSN_TIMER_CURRENT_RTC_SEC_L);
	sech = mac_ior(m, TSN_TIMER_CURRENT_RTC_SEC_H);

	ts->tv_sec = (((u64)sech << 32) | secl) & TSN_TIMER_MAX_SEC_MASK;
	ts->tv_nsec = nsec & TSN_TIMER_MAX_NSEC_MASK;
}

static void xlnx_tsn_rtc_offset_write(struct xlnx_tsn *sw,
				      const struct timespec64 *ts)
{
	struct xlnx_tsn_mac *m = &sw->mac[XLNX_TSN_PORT_MAC1];

	mac_iow(m, TSN_TIMER_RTC_OFFSET_SEC_H, upper_32_bits(ts->tv_sec));
	mac_iow(m, TSN_TIMER_RTC_OFFSET_SEC_L, lower_32_bits(ts->tv_sec));
	mac_iow(m, TSN_TIMER_RTC_OFFSET_NS, ts->tv_nsec);
}

static void xlnx_tsn_rtc_offset_read(struct xlnx_tsn *sw,
				     struct timespec64 *ts)
{
	struct xlnx_tsn_mac *m = &sw->mac[XLNX_TSN_PORT_MAC1];
	u32 secl, sech, nsec;

	secl = mac_ior(m, TSN_TIMER_RTC_OFFSET_SEC_L);
	sech = mac_ior(m, TSN_TIMER_RTC_OFFSET_SEC_H);
	nsec = mac_ior(m, TSN_TIMER_RTC_OFFSET_NS);

	ts->tv_sec = (((u64)sech << 32) | secl) & TSN_TIMER_MAX_SEC_MASK;
	ts->tv_nsec = nsec & TSN_TIMER_MAX_NSEC_MASK;
}

static int xlnx_tsn_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct xlnx_tsn *sw = container_of(ptp, struct xlnx_tsn, ptp_clock_info);
	u64 incval;

	/* adjust_by_scaled_ppm() returns u64 but the increment register is
	 * 32 bits, so clamp to U32_MAX to avoid overflow.
	 */
	incval = adjust_by_scaled_ppm(sw->rtc_value, scaled_ppm);
	if (incval > U32_MAX)
		incval = U32_MAX;

	guard(spinlock_irqsave)(&sw->reg_lock);
	mac_iow(&sw->mac[XLNX_TSN_PORT_MAC1], TSN_TIMER_RTC_INCREMENT,
		(u32)incval);

	return 0;
}

static int xlnx_tsn_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct xlnx_tsn *sw = container_of(ptp, struct xlnx_tsn, ptp_clock_info);
	struct timespec64 now, then = ns_to_timespec64(delta);

	guard(spinlock_irqsave)(&sw->reg_lock);

	xlnx_tsn_rtc_offset_read(sw, &now);
	now = timespec64_add(now, then);

	/* Stepping time backwards is fine and just lowers the offset. In
	 * practice the offset never goes negative. Reject it only as a
	 * safety net, since the offset register cannot store a negative value.
	 */
	if (now.tv_sec < 0)
		return -ERANGE;

	xlnx_tsn_rtc_offset_write(sw, &now);

	return 0;
}

static int xlnx_tsn_ptp_gettime(struct ptp_clock_info *ptp,
				struct timespec64 *ts)
{
	struct xlnx_tsn *sw = container_of(ptp, struct xlnx_tsn, ptp_clock_info);

	guard(spinlock_irqsave)(&sw->reg_lock);
	xlnx_tsn_tod_read(sw, ts);

	return 0;
}

static int xlnx_tsn_ptp_settime(struct ptp_clock_info *ptp,
				const struct timespec64 *ts)
{
	struct xlnx_tsn *sw = container_of(ptp, struct xlnx_tsn, ptp_clock_info);
	struct timespec64 delta, tod, offset, counter;

	guard(spinlock_irqsave)(&sw->reg_lock);

	xlnx_tsn_tod_read(sw, &tod);
	xlnx_tsn_rtc_offset_read(sw, &offset);
	counter = timespec64_sub(tod, offset);

	delta = timespec64_sub(*ts, counter);

	/* A real wall-clock time is always far above the free-running counter,
	 * so this never triggers in practice. Reject it only as a safety net,
	 * since the offset register cannot store a negative value.
	 */
	if (delta.tv_sec < 0)
		return -ERANGE;

	xlnx_tsn_rtc_offset_write(sw, &delta);

	return 0;
}

static int xlnx_tsn_ptp_enable(struct ptp_clock_info *ptp,
			       struct ptp_clock_request *rq, int on)
{
	struct xlnx_tsn *sw = container_of(ptp, struct xlnx_tsn, ptp_clock_info);

	switch (rq->type) {
	case PTP_CLK_REQ_PPS:
		WRITE_ONCE(sw->pps_enable, on ? 1 : 0);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static irqreturn_t xlnx_tsn_ptp_timer_isr(int irq, void *priv)
{
	struct ptp_clock_event event = { .type = PTP_CLOCK_PPS };
	struct xlnx_tsn *sw = priv;

	sw->countpulse++;
	if (sw->countpulse >= TSN_TIMER_PULSES_PER_PPS) {
		sw->countpulse = 0;
		if (sw->ptp_clock && READ_ONCE(sw->pps_enable))
			ptp_clock_event(sw->ptp_clock, &event);
	}

	mac_iow(&sw->mac[XLNX_TSN_PORT_MAC1], TSN_TIMER_INTERRUPT,
		TSN_TIMER_INT_CLEAR);

	return IRQ_HANDLED;
}

int xlnx_tsn_ptp_init(struct xlnx_tsn *sw)
{
	struct timespec64 ts;
	int ret;

	spin_lock_init(&sw->reg_lock);

	sw->ptp_timer_irq = of_irq_get_byname(sw->dev->of_node, "ptp_timer");
	if (sw->ptp_timer_irq <= 0)
		return dev_err_probe(sw->dev, sw->ptp_timer_irq ? : -ENXIO,
				     "failed to get ptp_timer IRQ\n");

	sw->ptp_clock_info.owner = THIS_MODULE;
	snprintf(sw->ptp_clock_info.name, sizeof(sw->ptp_clock_info.name),
		 "TSN PHC");
	sw->ptp_clock_info.max_adj = 999999999;
	sw->ptp_clock_info.pps = 1;
	sw->ptp_clock_info.adjfine = xlnx_tsn_ptp_adjfine;
	sw->ptp_clock_info.adjtime = xlnx_tsn_ptp_adjtime;
	sw->ptp_clock_info.gettime64 = xlnx_tsn_ptp_gettime;
	sw->ptp_clock_info.settime64 = xlnx_tsn_ptp_settime;
	sw->ptp_clock_info.enable = xlnx_tsn_ptp_enable;

	sw->ptp_clock = ptp_clock_register(&sw->ptp_clock_info, sw->dev);
	if (IS_ERR_OR_NULL(sw->ptp_clock)) {
		ret = sw->ptp_clock ? PTR_ERR(sw->ptp_clock) : -ENODEV;
		sw->ptp_clock = NULL;
		return dev_err_probe(sw->dev, ret,
				     "failed to register PTP clock\n");
	}

	sw->rtc_value = div_u64(NSEC_PER_SEC, TSN_TIMER_GTX_CLK_FREQ) <<
			TSN_TIMER_RTC_NS_SHIFT;
	mac_iow(&sw->mac[XLNX_TSN_PORT_MAC1], TSN_TIMER_RTC_INCREMENT,
		(u32)sw->rtc_value);

	ts = ktime_to_timespec64(ktime_get_real());
	xlnx_tsn_ptp_settime(&sw->ptp_clock_info, &ts);

	ret = request_irq(sw->ptp_timer_irq, xlnx_tsn_ptp_timer_isr, 0,
			  "xlnx-tsn-ptp-timer", sw);
	if (ret) {
		dev_err_probe(sw->dev, ret,
			      "failed to request ptp_timer IRQ %d\n",
			      sw->ptp_timer_irq);
		goto err_unregister_clock;
	}

	return 0;

err_unregister_clock:
	ptp_clock_unregister(sw->ptp_clock);
	sw->ptp_clock = NULL;
	return ret;
}

void xlnx_tsn_ptp_exit(struct xlnx_tsn *sw)
{
	if (!sw->ptp_clock)
		return;

	free_irq(sw->ptp_timer_irq, sw);
	ptp_clock_unregister(sw->ptp_clock);
	sw->ptp_clock = NULL;
}

static void memcpy_toio_32(struct xlnx_tsn_mac *m, unsigned long off,
			   const u8 *data, size_t len)
{
	while (len >= 4) {
		mac_iow(m, off, get_unaligned((const u32 *)data));
		off += 4;
		data += 4;
		len -= 4;
	}

	if (len) {
		u32 leftover = 0;
		u8 *dst = (u8 *)&leftover;

		while (len--)
			*dst++ = *data++;
		mac_iow(m, off, leftover);
	}
}

static void memcpy_fromio_32(struct xlnx_tsn_mac *m, unsigned long off,
			     u8 *data, size_t len)
{
	while (len >= 4) {
		put_unaligned(mac_ior(m, off), (u32 *)data);
		off += 4;
		data += 4;
		len -= 4;
	}

	if (len) {
		u32 leftover = mac_ior(m, off);
		u8 *src = (u8 *)&leftover;

		while (len--)
			*data++ = *src++;
	}
}

static void xlnx_tsn_read_tstamp(struct xlnx_tsn_mac *m,
				 struct skb_shared_hwtstamps *hwtstamps,
				 unsigned int off)
{
	u32 captured_ns, captured_sec;

	memset(hwtstamps, 0, sizeof(*hwtstamps));

	captured_ns = mac_ior(m, off + 4);
	captured_sec = mac_ior(m, off);

	hwtstamps->hwtstamp = ktime_set(captured_sec, captured_ns);
}

void xlnx_tsn_ptp_tx(struct dsa_port *dp, struct sk_buff *skb)
{
	struct xlnx_tsn *sw = dp->ds->priv;
	u32 frame_waiting, cmd1, cmd2 = 0;
	struct xlnx_tsn_mac *m;
	u8 free_index;

	m = &sw->mac[dp->index];

	if (unlikely(skb->len > TSN_PTP_TX_MAX_FRAME_SIZE)) {
		dev_kfree_skb_any(skb);
		return;
	}

	scoped_guard(spinlock_irqsave, &m->ptp_tx_lock) {
		frame_waiting = FIELD_GET(TSN_PTP_TX_FRAME_WAITING_MASK,
					  mac_ior(m, TSN_PTP_TX_CONTROL_OFFSET));
		if (frame_waiting & TSN_PTP_TX_BUFFERS_FULL_MASK) {
			dev_kfree_skb_any(skb);
			return;
		}

		free_index = fls(frame_waiting);
		cmd1 = skb->len;

		mac_iow(m, TSN_PTP_TX_BUFFER_OFFSET(free_index), cmd1);
		mac_iow(m, TSN_PTP_TX_BUFFER_OFFSET(free_index) +
			TSN_PTP_TX_BUFFER_CMD2_FIELD, cmd2);
		memcpy_toio_32(m,
			       TSN_PTP_TX_BUFFER_OFFSET(free_index) +
			       TSN_PTP_TX_CMD_FIELD_LEN,
			       skb->data, skb->len);

		skb->cb[0] = free_index;
		__skb_queue_tail(&m->ptp_txq, skb);

		if (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP)
			skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;

		skb_tx_timestamp(skb);
		mac_iow(m, TSN_PTP_TX_CONTROL_OFFSET, BIT(free_index));
	}
}

static void xlnx_tsn_ptp_recv(struct xlnx_tsn *sw, int port)
{
	struct net_device *user = dsa_to_port(&sw->ds, port)->user;
	struct xlnx_tsn_mac *m = &sw->mac[port];
	unsigned long frame_base;
	struct sk_buff *skb;
	u16 msg_len;
	u8 msg_type;

	if (!user || !netif_running(user))
		return;

	while ((m->ptp_rx_hw_pointer & 0xf) != (m->ptp_rx_sw_pointer & 0xf)) {
		m->ptp_rx_sw_pointer++;

		frame_base = TSN_PTP_RX_BASE_OFFSET +
			     (m->ptp_rx_sw_pointer & 0xf) *
			     TSN_PTP_RX_HWBUF_SIZE;

		skb = netdev_alloc_skb(user, TSN_PTP_RX_FRAME_SIZE);
		if (!skb) {
			DEV_STATS_INC(user, rx_dropped);
			continue;
		}

		memcpy_fromio_32(m, frame_base, skb->data,
				 TSN_PTP_RX_FRAME_SIZE);

		msg_type = *(u8 *)(skb->data + ETH_HLEN) & 0xf;
		msg_len = get_unaligned_be16(skb->data + ETH_HLEN + 2);

		if (msg_len + ETH_HLEN > TSN_PTP_RX_FRAME_SIZE) {
			dev_kfree_skb_any(skb);
			DEV_STATS_INC(user, rx_length_errors);
			continue;
		}

		skb_put(skb, msg_len + ETH_HLEN);
		skb->protocol = eth_type_trans(skb, user);
		skb->ip_summed = CHECKSUM_UNNECESSARY;

		if (READ_ONCE(m->hwtstamp_rx_filter) != HWTSTAMP_FILTER_NONE &&
		    !(msg_type & TSN_PTP_MSG_TYPE_MASK))
			xlnx_tsn_read_tstamp(m, skb_hwtstamps(skb),
					     frame_base +
					     TSN_PTP_HW_TSTAMP_OFFSET);

		dev_sw_netstats_rx_add(user, skb->len);
		netif_rx(skb);
	}
}

static irqreturn_t xlnx_tsn_ptp_rx_isr(int irq, void *data)
{
	struct xlnx_tsn_mac *m = data;
	struct xlnx_tsn *sw = m->sw;
	int port = m - sw->mac;

	m->ptp_rx_hw_pointer = FIELD_GET(TSN_PTP_RX_PACKET_FIELD_MASK,
					 mac_ior(m, TSN_PTP_RX_CONTROL_OFFSET));
	xlnx_tsn_ptp_recv(sw, port);

	return IRQ_HANDLED;
}

static void xlnx_tsn_tx_tstamp_work(struct work_struct *work)
{
	struct xlnx_tsn_mac *m = container_of(work, struct xlnx_tsn_mac,
					      tx_tstamp_work);
	struct skb_shared_hwtstamps hwtstamps;
	unsigned long ts_off;
	struct sk_buff *skb;
	u8 tx_packet, index;

	guard(spinlock_irqsave)(&m->ptp_tx_lock);

	tx_packet = FIELD_GET(TSN_PTP_TX_PACKET_FIELD_MASK,
			      mac_ior(m, TSN_PTP_TX_CONTROL_OFFSET));

	while ((skb = __skb_dequeue(&m->ptp_txq)) != NULL) {
		index = skb->cb[0];

		/* HW writes ascending slot indices into the TX status field
		 * as frames depart. Any queued skb with index > tx_packet
		 * has not been timestamped yet, so requeue it and stop.
		 */
		if (index > tx_packet) {
			__skb_queue_head(&m->ptp_txq, skb);
			break;
		}

		ts_off = TSN_PTP_TX_BUFFER_OFFSET(index) +
			 TSN_PTP_HW_TSTAMP_OFFSET;

		if (skb_shinfo(skb)->tx_flags & SKBTX_IN_PROGRESS) {
			xlnx_tsn_read_tstamp(m, &hwtstamps, ts_off);
			skb_tstamp_tx(skb, &hwtstamps);
		}
		consume_skb(skb);
	}
}

static irqreturn_t xlnx_tsn_ptp_tx_isr(int irq, void *data)
{
	struct xlnx_tsn_mac *m = data;

	mac_ior(m, TSN_PTP_TX_CONTROL_OFFSET);
	schedule_work(&m->tx_tstamp_work);

	return IRQ_HANDLED;
}

int xlnx_tsn_port_ptp_init(struct xlnx_tsn *sw, int port,
			   const char *rx_name, const char *tx_name)
{
	struct xlnx_tsn_mac *m = &sw->mac[port];
	int ret;

	skb_queue_head_init(&m->ptp_txq);
	spin_lock_init(&m->ptp_tx_lock);
	INIT_WORK(&m->tx_tstamp_work, xlnx_tsn_tx_tstamp_work);
	m->ptp_rx_hw_pointer = 0;
	m->ptp_rx_sw_pointer = 0xff;
	m->hwtstamp_tx_type = HWTSTAMP_TX_OFF;
	m->hwtstamp_rx_filter = HWTSTAMP_FILTER_NONE;

	m->ptp_rx_irq = of_irq_get_byname(sw->dev->of_node, rx_name);
	if (m->ptp_rx_irq <= 0)
		return dev_err_probe(sw->dev, m->ptp_rx_irq ? : -ENXIO,
				     "failed to get %s IRQ\n", rx_name);

	m->ptp_tx_irq = of_irq_get_byname(sw->dev->of_node, tx_name);
	if (m->ptp_tx_irq <= 0)
		return dev_err_probe(sw->dev, m->ptp_tx_irq ? : -ENXIO,
				     "failed to get %s IRQ\n", tx_name);

	mac_iow(m, TSN_PTP_RX_CONTROL_OFFSET, TSN_PTP_RX_PACKET_CLEAR);

	ret = request_irq(m->ptp_rx_irq, xlnx_tsn_ptp_rx_isr, 0, rx_name, m);
	if (ret)
		return dev_err_probe(sw->dev, ret,
				     "failed to request %s IRQ %d\n",
				     rx_name, m->ptp_rx_irq);

	ret = request_irq(m->ptp_tx_irq, xlnx_tsn_ptp_tx_isr, 0, tx_name, m);
	if (ret) {
		free_irq(m->ptp_rx_irq, m);
		return dev_err_probe(sw->dev, ret,
				     "failed to request %s IRQ %d\n",
				     tx_name, m->ptp_tx_irq);
	}

	return 0;
}

void xlnx_tsn_port_ptp_exit(struct xlnx_tsn *sw, int port)
{
	struct xlnx_tsn_mac *m = &sw->mac[port];
	struct sk_buff *skb;

	if (m->ptp_tx_irq > 0)
		free_irq(m->ptp_tx_irq, m);

	if (m->ptp_rx_irq > 0)
		free_irq(m->ptp_rx_irq, m);

	cancel_work_sync(&m->tx_tstamp_work);

	scoped_guard(spinlock_irqsave, &m->ptp_tx_lock)
		while ((skb = __skb_dequeue(&m->ptp_txq)) != NULL)
			dev_kfree_skb_any(skb);
}

int xlnx_tsn_port_hwtstamp_get(struct dsa_switch *ds, int port,
			       struct kernel_hwtstamp_config *config)
{
	struct xlnx_tsn *sw = ds->priv;
	struct xlnx_tsn_mac *m;

	m = &sw->mac[port];

	if (port == XLNX_TSN_CPU_PORT)
		return -EOPNOTSUPP;

	config->tx_type = m->hwtstamp_tx_type;
	config->rx_filter = m->hwtstamp_rx_filter;

	return 0;
}

int xlnx_tsn_port_hwtstamp_set(struct dsa_switch *ds, int port,
			       struct kernel_hwtstamp_config *config,
			       struct netlink_ext_ack *extack)
{
	struct xlnx_tsn *sw = ds->priv;
	struct xlnx_tsn_mac *m;

	m = &sw->mac[port];

	if (port == XLNX_TSN_CPU_PORT)
		return -EOPNOTSUPP;

	switch (config->tx_type) {
	case HWTSTAMP_TX_OFF:
	case HWTSTAMP_TX_ON:
		break;
	default:
		return -ERANGE;
	}

	/* The per-MAC RX filter is left at its power-on default, which
	 * captures L2 gPTP event frames (ethertype 0x88f7) only. PTP over
	 * UDP is not supported. The filter cannot narrow by message type,
	 * so any L2 PTPv2 event request is promoted to
	 * HWTSTAMP_FILTER_PTP_V2_L2_EVENT.
	 */
	switch (config->rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		break;
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
		config->rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT;
		break;
	default:
		return -ERANGE;
	}

	m->hwtstamp_tx_type = config->tx_type;
	WRITE_ONCE(m->hwtstamp_rx_filter, config->rx_filter);

	return 0;
}

int xlnx_tsn_get_ts_info(struct dsa_switch *ds, int port,
			 struct kernel_ethtool_ts_info *info)
{
	struct xlnx_tsn *sw = ds->priv;

	info->phc_index = sw->ptp_clock ? ptp_clock_index(sw->ptp_clock) : -1;
	info->so_timestamping = SOF_TIMESTAMPING_TX_HARDWARE |
				SOF_TIMESTAMPING_RX_HARDWARE |
				SOF_TIMESTAMPING_RAW_HARDWARE;
	info->tx_types = BIT(HWTSTAMP_TX_OFF) | BIT(HWTSTAMP_TX_ON);
	info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) |
			    BIT(HWTSTAMP_FILTER_PTP_V2_L2_EVENT);

	return 0;
}
