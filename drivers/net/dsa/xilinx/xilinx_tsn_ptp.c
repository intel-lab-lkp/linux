// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD/Xilinx TSN Endpoint Ethernet MAC DSA switch driver:
 * PTP hardware clock.
 */

#include <linux/cleanup.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/of_irq.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/spinlock.h>
#include <linux/time64.h>
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
