// SPDX-License-Identifier: GPL-2.0
#include "xilinx_tsn.h"

/**
 * rtc_iow - Write to PTP RTC timer register
 * @timer:	Pointer to TSN PTP timer structure
 * @reg:	Register offset
 * @val:	Value to write
 *
 * This function writes the desired value into the corresponding TSN
 * PTP register.
 */
static inline void rtc_iow(struct tsn_ptp_timer *timer, u32 reg, u32 val)
{
	iowrite32(val, timer->regs + reg);
}

/**
 * rtc_ior - Read from PTP RTC timer register
 * @timer:	Pointer to TSN PTP timer structure
 * @reg:	Register offset
 *
 * This function reads a value from the corresponding TSN PTP
 * register.
 *
 * Return:	Register value
 */
static inline u32 rtc_ior(struct tsn_ptp_timer *timer, u32 reg)
{
	return ioread32(timer->regs + reg);
}

/**
 * tsn_tod_read - Read current time-of-day from RTC timer
 * @timer:	Pointer to TSN PTP timer structure
 * @ts:		Pointer to timespec64 to store current time
 *
 * Reads the 64-bit seconds (high + low) and nanoseconds from the RTC current
 * time registers. Values are masked to valid ranges.
 */
static void tsn_tod_read(struct tsn_ptp_timer *timer,
			 struct timespec64 *ts)
{
	u32 secl, sech, nsec;

	nsec = rtc_ior(timer, TSN_TIMER_CURRENT_RTC_NS);
	secl = rtc_ior(timer, TSN_TIMER_CURRENT_RTC_SEC_L);
	sech = rtc_ior(timer, TSN_TIMER_CURRENT_RTC_SEC_H);

	ts->tv_sec = (((u64)sech << 32) | secl) & TSN_TIMER_MAX_SEC_MASK;
	ts->tv_nsec = nsec & TSN_TIMER_MAX_NSEC_MASK;
}

/**
 * tsn_rtc_offset_write - Write time offset to RTC offset registers
 * @timer:	Pointer to TSN PTP timer structure
 * @ts:		Pointer to timespec64 with offset value to write
 *
 */
static void tsn_rtc_offset_write(struct tsn_ptp_timer *timer,
				 const struct timespec64 *ts)
{
	rtc_iow(timer, TSN_TIMER_RTC_OFFSET_SEC_H, upper_32_bits(ts->tv_sec));
	rtc_iow(timer, TSN_TIMER_RTC_OFFSET_SEC_L, lower_32_bits(ts->tv_sec));
	rtc_iow(timer, TSN_TIMER_RTC_OFFSET_NS, ts->tv_nsec);
}

/**
 * tsn_rtc_offset_read - Read time offset from RTC offset registers
 * @timer:	Pointer to TSN PTP timer structure
 * @ts:		Pointer to timespec64 to store offset value
 *
 * Reads the current RTC offset value from the offset registers.
 * Values are masked to valid ranges.
 */
static void tsn_rtc_offset_read(struct tsn_ptp_timer *timer,
				struct timespec64 *ts)
{
	u32 secl, sech, nsec;

	secl = rtc_ior(timer, TSN_TIMER_RTC_OFFSET_SEC_L);
	sech = rtc_ior(timer, TSN_TIMER_RTC_OFFSET_SEC_H);
	nsec = rtc_ior(timer, TSN_TIMER_RTC_OFFSET_NS);

	ts->tv_sec = (((u64)sech << 32) | secl) & TSN_TIMER_MAX_SEC_MASK;
	ts->tv_nsec = nsec & TSN_TIMER_MAX_NSEC_MASK;
}

/**
 * tsn_ptp_adjfine - Adjust PTP clock frequency
 * @ptp:	Pointer to PTP clock info structure
 * @scaled_ppm:	Frequency adjustment in scaled parts-per-million
 *
 * Adjusts the RTC increment value to fine-tune the clock frequency.
 * Uses adjust_by_scaled_ppm() helper to calculate the new increment value
 * based on the base RTC value (calculated from 125 MHz GTX clock).
 *
 * Return:	0 on success
 */
static int tsn_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct tsn_ptp_timer *timer = container_of(ptp,
						   struct tsn_ptp_timer,
						   ptp_clock_info);
	u32 incval;

	incval = adjust_by_scaled_ppm(timer->rtc_value, scaled_ppm);
	rtc_iow(timer, TSN_TIMER_RTC_INCREMENT, incval);

	return 0;
}

/**
 * tsn_ptp_adjtime - Adjust PTP clock time by offset
 * @ptp:	Pointer to PTP clock info structure
 * @delta:	Time offset in nanoseconds (positive or negative)
 *
 * Adjusts the RTC time by adding the specified delta offset.
 * Reads the current offset, adds the delta to it, and writes back.
 *
 * Return:	0 on success
 */
static int tsn_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct tsn_ptp_timer *timer = container_of(ptp, struct tsn_ptp_timer,
						   ptp_clock_info);
	struct timespec64 now, then = ns_to_timespec64(delta);

	guard(spinlock_irqsave)(&timer->reg_lock);

	tsn_rtc_offset_read(timer, &now);
	now = timespec64_add(now, then);
	tsn_rtc_offset_write(timer, &now);

	return 0;
}

/**
 * tsn_ptp_gettime - Get current PTP clock time
 * @ptp:	Pointer to PTP clock info structure
 * @ts:		Pointer to timespec64 to receive current time
 *
 * Reads the current time-of-day from the RTC timer.
 *
 * Return:	0 on success
 */
static int tsn_ptp_gettime(struct ptp_clock_info *ptp,
			   struct timespec64 *ts)
{
	struct tsn_ptp_timer *timer = container_of(ptp, struct tsn_ptp_timer,
						   ptp_clock_info);

	guard(spinlock_irqsave)(&timer->reg_lock);
	tsn_tod_read(timer, ts);

	return 0;
}

/**
 * tsn_ptp_settime - Set PTP clock time
 * @ptp:	Pointer to PTP clock info structure
 * @ts:		Pointer to timespec64 with new time to set
 *
 * Return:	0 on success, -EINVAL for invalid timestamp
 */
static int tsn_ptp_settime(struct ptp_clock_info *ptp,
			   const struct timespec64 *ts)
{
	struct tsn_ptp_timer *timer = container_of(ptp, struct tsn_ptp_timer,
						   ptp_clock_info);
	struct timespec64 delta, tod, offset;

	if (!ts || ts->tv_nsec < 0 || ts->tv_nsec >= NSEC_PER_SEC)
		return -EINVAL;

	guard(spinlock_irqsave)(&timer->reg_lock);

	/* Zero the offset first */
	offset.tv_sec = 0;
	offset.tv_nsec = 0;
	tsn_rtc_offset_write(timer, &offset);

	/* Get current timer value */
	tsn_tod_read(timer, &tod);

	/* Calculate delta */
	delta = timespec64_sub(*ts, tod);

	/* Don't write negative offset */
	if (delta.tv_sec < 0 || (delta.tv_sec == 0 && delta.tv_nsec < 0)) {
		delta.tv_sec = 0;
		delta.tv_nsec = 0;
	}

	tsn_rtc_offset_write(timer, &delta);

	return 0;
}

/**
 * tsn_ptp_enable - Enable or disable PPS output
 * @ptp:	Pointer to PTP clock info structure
 * @rq:		Pointer to PTP clock request
 * @on:		1 to enable, 0 to disable
 *
 * Enables or disables the PPS (pulse-per-second) event delivery.
 * The TSN IP generates 128 pulses per second, and this function controls
 * whether those pulses are reported to the PTP subsystem via ptp_clock_event().
 * Only supports PTP_CLK_REQ_PPS request type.
 *
 * Return:	0 on success, -EOPNOTSUPP for unsupported request types
 */
static int tsn_ptp_enable(struct ptp_clock_info *ptp,
			  struct ptp_clock_request *rq, int on)
{
	struct tsn_ptp_timer *timer = container_of(ptp, struct tsn_ptp_timer,
						   ptp_clock_info);

	switch (rq->type) {
	case PTP_CLK_REQ_PPS:
		timer->pps_enable = on ? 1 : 0;
		return 0;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

/**
 * tsn_ptp_timer_isr - PTP timer interrupt handler
 * @irq:	Interrupt number
 * @priv:	Pointer to tsn_ptp_timer structure
 *
 * Handles PTP timer interrupts for PPS (pulse-per-second) events.
 * The TSN IP generates 128 pulses per second. This ISR counts those pulses
 * and delivers a PTP_CLOCK_PPS event once per second (every 128 pulses) if
 * PPS is enabled via tsn_ptp_enable().
 *
 * Return:	IRQ_HANDLED
 */
static irqreturn_t tsn_ptp_timer_isr(int irq, void *priv)
{
	struct tsn_ptp_timer *timer = priv;
	struct ptp_clock_event event;

	event.type = PTP_CLOCK_PPS;

	timer->countpulse++;
	if (timer->countpulse >= PULSESIN1PPS) {
		timer->countpulse = 0;
		if (timer->ptp_clock && timer->pps_enable)
			ptp_clock_event(timer->ptp_clock, &event);
	}

	/* Clear interrupt */
	rtc_iow(timer, TSN_TIMER_INTERRUPT, BIT(TSN_TIMER_INT_SHIFT));

	return IRQ_HANDLED;
}

/**
 * tsn_ptp_timer_init - Initialize PTP timer and register PHC
 * @emac:	Pointer to TSN EMAC structure
 * @emac_np:	Pointer to EMAC device tree node
 *
 * The PTP timer is shared globally - only initialized once for TEMAC1.
 * TEMAC2 will skip initialization and share the same PHC index.
 *
 * Return:	0 on success, negative error code on failure
 */
int tsn_ptp_timer_init(struct tsn_emac *emac, struct device_node *emac_np)
{
	struct tsn_priv *common = emac->common;
	struct tsn_ptp_timer *timer = &common->ptp_timer;
	struct device *dev = common->dev;
	struct timespec64 ts;
	int ret;

	if (timer->ptp_clock) {
		dev_info(dev, "PTP timer already initialized (PHC: %d)\n",
			 common->phc_index);
		return 0;
	}

	memset(timer, 0, sizeof(*timer));
	timer->dev = dev;
	timer->irq = -1;

	timer->regs = emac->regs + TSN_PTP_TIMER_OFFSET;

	spin_lock_init(&timer->reg_lock);

	timer->irq = of_irq_get_byname(emac_np, "interrupt_ptp_timer");
	if (timer->irq < 0) {
		timer->irq = platform_get_irq_byname(common->pdev, "interrupt_ptp_timer");
		if (timer->irq < 0) {
			dev_err(dev, "Failed to get PTP timer interrupt: %d\n",
				timer->irq);
			ret = timer->irq;
			goto err_cleanup;
		}
	}

	ret = devm_request_irq(dev, timer->irq, tsn_ptp_timer_isr, 0,
			       "tsn_ptp_timer", timer);
	if (ret) {
		dev_err(dev, "Failed to request PTP timer IRQ %d: %d\n",
			timer->irq, ret);
		goto err_cleanup;
	}

	/* Setup PTP clock info */
	timer->ptp_clock_info.owner = THIS_MODULE;
	snprintf(timer->ptp_clock_info.name,
		 sizeof(timer->ptp_clock_info.name), "TSN PTP");
	timer->ptp_clock_info.max_adj = 999999999;
	timer->ptp_clock_info.n_ext_ts = 0;
	timer->ptp_clock_info.pps = 1;
	timer->ptp_clock_info.adjfine = tsn_ptp_adjfine;
	timer->ptp_clock_info.adjtime = tsn_ptp_adjtime;
	timer->ptp_clock_info.gettime64 = tsn_ptp_gettime;
	timer->ptp_clock_info.settime64 = tsn_ptp_settime;
	timer->ptp_clock_info.enable = tsn_ptp_enable;

	/* Register PTP clock */
	timer->ptp_clock = ptp_clock_register(&timer->ptp_clock_info, dev);
	if (IS_ERR(timer->ptp_clock)) {
		ret = PTR_ERR(timer->ptp_clock);
		dev_err(dev, "Failed to register PTP clock: %d\n", ret);
		timer->ptp_clock = NULL;
		goto err_cleanup;
	}

	/* In the TSN IP Core, RTC clock is connected to gtx_clk which is
	 * 125 MHz. This is specified in the TSN PG and is not configurable.
	 *
	 * Calculating the RTC Increment Value once and storing it in
	 * timer->rtc_value to prevent recalculating it each time the PTP
	 * frequency is adjusted in xlnx_ptp_adjfine()
	 */
	timer->rtc_value = (div_u64(NSEC_PER_SEC, TSN_TIMER_GTX_CLK_FREQ) <<
			    TSN_TIMER_RTC_NS_SHIFT);

	rtc_iow(timer, TSN_TIMER_RTC_INCREMENT, timer->rtc_value);

	ts = ktime_to_timespec64(ktime_get_real());
	tsn_ptp_settime(&timer->ptp_clock_info, &ts);

	/* Store PHC index */
	common->phc_index = ptp_clock_index(timer->ptp_clock);

	dev_info(dev, "PTP timer initialized (PHC: %d, IRQ: %d, offset: 0x%x)\n",
		 common->phc_index, timer->irq, TSN_PTP_TIMER_OFFSET);

	return 0;

err_cleanup:
	timer->irq = -1;
	common->phc_index = -1;
	return ret;
}

/**
 * tsn_ptp_timer_exit - Cleanup PTP timer and unregister PHC
 * @emac:	Pointer to TSN EMAC structure
 *
 * Unregisters the PTP clock from the kernel PTP subsystem and
 * cleans up the PTP timer state. Sets phc_index back to -1.
 * The interrupt is automatically freed by devm_request_irq().
 */
void tsn_ptp_timer_exit(struct tsn_emac *emac)
{
	struct tsn_priv *common = emac->common;
	struct tsn_ptp_timer *timer = &common->ptp_timer;

	if (!timer->ptp_clock)
		return;

	ptp_clock_unregister(timer->ptp_clock);
	dev_info(common->dev, "PTP timer unregistered (PHC: %d)\n",
		 common->phc_index);

	timer->ptp_clock = NULL;
	common->phc_index = -1;
}
