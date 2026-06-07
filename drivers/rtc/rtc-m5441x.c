// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RTC driver for the Freescale MCF5441x on-chip "robust" RTC.
 *
 * Exposes the time/calendar and alarm, plus the 2KB battery-backed
 * standby RAM as an nvmem device. The standby RAM survives main-power
 * loss while VSTBY_RTC is supplied, which makes it usable to persist a
 * reset/reboot counter across power cycles.
 *
 * Register layout and the base-2112 year encoding are taken from the
 * MCF54418 reference manual (chapter "Robust Real Time Clock").
 *
 * Copyright (C) 2010-2011 Freescale Semiconductor, Inc.
 * Copyright (C) 2026 Jean-Michel Hautbois <jeanmichel.hautbois@yoseli.org>
 *
 * Based on the out-of-tree Freescale 3.0.x rtc-m5441x driver by
 * Lanttor Guo <Lanttor.Guo@freescale.com>, rewritten for the current
 * RTC and nvmem frameworks.
 */
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/rtc.h>
#include <linux/spinlock.h>

/* Register offsets from the RTC base. */
#define M5441X_RTC_YEARMON	0x00
#define M5441X_RTC_DAYS		0x02
#define M5441X_RTC_HOURMIN	0x04
#define M5441X_RTC_SECONDS	0x06
#define M5441X_RTC_ALM_YRMON	0x08
#define M5441X_RTC_ALM_DAYS	0x0a
#define M5441X_RTC_ALM_HM	0x0c
#define M5441X_RTC_ALM_SEC	0x0e
#define M5441X_RTC_CR		0x10
#define M5441X_RTC_SR		0x12
#define M5441X_RTC_ISR		0x14
#define M5441X_RTC_IER		0x16
#define M5441X_RTC_CFG_DATA	0x20

/* RTC_CR bits. Only WE is freely writable; the rest need the WE knock. */
#define M5441X_RTC_CR_RSVD	0x8000	/* bit 15, reserved, must be set */
#define M5441X_RTC_CR_BCDEN	0x0080	/* 0 = binary, 1 = BCD time/date */
#define M5441X_RTC_CR_DSTEN	0x0040	/* daylight-saving auto-adjust */
#define M5441X_RTC_CR_AM_MASK	0x000c	/* alarm match field */
#define M5441X_RTC_CR_AM_FULL	0x000c	/* match s,m,h,day,month,year */
#define M5441X_RTC_CR_WE_MASK	0x0003	/* write-enable knock field */

#define M5441X_RTC_SR_INVAL	0x0001	/* time invalid / changing */
#define M5441X_RTC_SR_WPE	0x0010	/* write protection enabled */
#define M5441X_RTC_ISR_ALM	0x0004	/* alarm interrupt status/enable */
#define M5441X_RTC_IER_RSVD	0x0001	/* IER/ISR bit 0 reserved, must be set */
#define M5441X_RTC_CFG_OSCEN	0x0008	/* oscillator enable */

/* Battery-backed standby RAM: 2KB at this offset within the block. */
#define M5441X_RTC_SRAM_OFFSET	0x40
#define M5441X_RTC_SRAM_SIZE	2048
#define M5441X_RTC_SRAM_CHUNK	64	/* lock-drop granularity for SRAM I/O */

#define M5441X_RTC_YEAR_BASE	2112

struct m5441x_rtc {
	struct rtc_device *rtc;
	void __iomem *base;
	spinlock_t lock;	/* serialises register access + WP window */
	bool osc_dead;		/* 32kHz oscillator never started; no timekeeping */
};

static inline u16 rtc_rd(struct m5441x_rtc *p, unsigned int reg)
{
	return ioread16be(p->base + reg);
}

static inline void rtc_wr(struct m5441x_rtc *p, unsigned int reg, u16 val)
{
	iowrite16be(val, p->base + reg);
}

/*
 * Issue the RTC_CR[WE] knock (00 -> 01 -> 11 -> 10) that opens the
 * write-protection window. Writes to the registers and the standby RAM
 * are rejected unless this sequence has been issued; the space then
 * stays writable for ~2 seconds. Caller holds p->lock.
 */
static void m5441x_rtc_knock(struct m5441x_rtc *p)
{
	rtc_wr(p, M5441X_RTC_CR, 0x0000);
	rtc_wr(p, M5441X_RTC_CR, 0x0001);
	rtc_wr(p, M5441X_RTC_CR, 0x0003);
	rtc_wr(p, M5441X_RTC_CR, 0x0002);
}

/* Open the write window only if protection is currently enabled. */
static void m5441x_rtc_unlock(struct m5441x_rtc *p)
{
	if (rtc_rd(p, M5441X_RTC_SR) & M5441X_RTC_SR_WPE)
		m5441x_rtc_knock(p);
}

/*
 * Write the data bits of RTC_CR (BCDEN/DSTEN/AM). Caller holds p->lock
 * and must have opened the write window first. The reserved bit 15 is
 * kept set and the WE field kept clear so the write updates the data
 * bits without re-arming write protection (a lone WE=10 would).
 */
static void m5441x_rtc_write_cr(struct m5441x_rtc *p, u16 val)
{
	val &= ~M5441X_RTC_CR_WE_MASK;
	val |= M5441X_RTC_CR_RSVD;
	rtc_wr(p, M5441X_RTC_CR, val);
}

/*
 * The time counters are unstable for an oscillator cycle either side of
 * the one-second boundary. RTC_SR[INVAL] flags this; reads during the
 * window return 0xffff and writes are nullified. Poll until it clears.
 * The window is only a couple of 32kHz cycles (~60us), so bound the
 * busy-wait tightly. This runs with the lock held and interrupts off, so
 * poll atomically (no sleeping). Caller holds p->lock.
 */
static int m5441x_rtc_wait_valid(struct m5441x_rtc *p)
{
	u16 sr;
	int ret;

	ret = read_poll_timeout_atomic(rtc_rd, sr, !(sr & M5441X_RTC_SR_INVAL),
				       10, 100, false, p, M5441X_RTC_SR);

	return ret ? -EIO : 0;
}

static int m5441x_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct m5441x_rtc *p = dev_get_drvdata(dev);
	u16 yearmon, days, hourmin, seconds;
	unsigned int tries = 3;
	unsigned long flags;
	int ret;

	/*
	 * Without a running oscillator the counters are frozen; refuse the
	 * read rather than hand userspace a stale time that hwclock --hctosys
	 * would propagate into the system clock.
	 */
	if (p->osc_dead)
		return -ENODATA;

	/*
	 * INVAL only guarantees the counters are stable at the moment it is
	 * checked; it does not freeze them across the four reads. If a 1Hz
	 * tick re-asserts INVAL mid-burst the fields can straddle a second
	 * boundary (e.g. 23:59:59 -> 00:00:00), so re-check it afterwards and
	 * retry the whole burst on observed re-entry.
	 */
	spin_lock_irqsave(&p->lock, flags);
	do {
		ret = m5441x_rtc_wait_valid(p);
		if (ret)
			break;
		yearmon = rtc_rd(p, M5441X_RTC_YEARMON);
		days    = rtc_rd(p, M5441X_RTC_DAYS);
		hourmin = rtc_rd(p, M5441X_RTC_HOURMIN);
		seconds = rtc_rd(p, M5441X_RTC_SECONDS);
		/* Stable across the whole burst -> the snapshot is coherent. */
		if (!(rtc_rd(p, M5441X_RTC_SR) & M5441X_RTC_SR_INVAL))
			break;
		ret = -EAGAIN;
	} while (--tries);
	spin_unlock_irqrestore(&p->lock, flags);

	if (ret)
		return ret;

	tm->tm_year = M5441X_RTC_YEAR_BASE + (s8)((yearmon >> 8) & 0xff) - 1900;
	tm->tm_mon  = (yearmon & 0xff) - 1;
	tm->tm_mday = days & 0xff;
	tm->tm_wday = (days >> 8) & 0x7;
	tm->tm_hour = (hourmin >> 8) & 0x1f;
	tm->tm_min  = hourmin & 0x3f;
	tm->tm_sec  = seconds & 0x3f;

	return 0;
}

static int m5441x_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct m5441x_rtc *p = dev_get_drvdata(dev);
	s8 yoff = (tm->tm_year + 1900) - M5441X_RTC_YEAR_BASE;
	unsigned int tries = 3;
	unsigned long flags;
	int ret;

	/* A frozen oscillator would never latch the new time; reject it. */
	if (p->osc_dead)
		return -ENODATA;

	/*
	 * Writes are nullified while INVAL is asserted. wait_valid() clears
	 * the path before the burst, but the knock plus four writes can cross
	 * a second boundary on a slow RTC bus, silently dropping fields.
	 * Re-check INVAL afterwards and rewrite on observed re-entry.
	 */
	spin_lock_irqsave(&p->lock, flags);
	do {
		ret = m5441x_rtc_wait_valid(p);
		if (ret)
			break;
		m5441x_rtc_unlock(p);
		rtc_wr(p, M5441X_RTC_YEARMON,
		       ((u16)((u8)yoff) << 8) | ((tm->tm_mon + 1) & 0xff));
		rtc_wr(p, M5441X_RTC_DAYS,
		       ((tm->tm_wday & 0x7) << 8) | (tm->tm_mday & 0x1f));
		rtc_wr(p, M5441X_RTC_HOURMIN,
		       ((tm->tm_hour & 0x1f) << 8) | (tm->tm_min & 0x3f));
		rtc_wr(p, M5441X_RTC_SECONDS, tm->tm_sec & 0x3f);
		/* No boundary crossed mid-burst -> the writes all took. */
		if (!(rtc_rd(p, M5441X_RTC_SR) & M5441X_RTC_SR_INVAL))
			break;
		ret = -EAGAIN;
	} while (--tries);
	spin_unlock_irqrestore(&p->lock, flags);

	return ret;
}

static int m5441x_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct m5441x_rtc *p = dev_get_drvdata(dev);
	u16 yearmon, days, hourmin, seconds, ier, isr;
	unsigned long flags;

	spin_lock_irqsave(&p->lock, flags);
	yearmon = rtc_rd(p, M5441X_RTC_ALM_YRMON);
	days    = rtc_rd(p, M5441X_RTC_ALM_DAYS);
	hourmin = rtc_rd(p, M5441X_RTC_ALM_HM);
	seconds = rtc_rd(p, M5441X_RTC_ALM_SEC);
	ier     = rtc_rd(p, M5441X_RTC_IER);
	isr     = rtc_rd(p, M5441X_RTC_ISR);
	spin_unlock_irqrestore(&p->lock, flags);

	alrm->time.tm_year = M5441X_RTC_YEAR_BASE +
			     (s8)((yearmon >> 8) & 0xff) - 1900;
	alrm->time.tm_mon  = (yearmon & 0xff) - 1;
	alrm->time.tm_mday = days & 0xff;
	alrm->time.tm_hour = (hourmin >> 8) & 0x1f;
	alrm->time.tm_min  = hourmin & 0x3f;
	alrm->time.tm_sec  = seconds & 0x3f;
	alrm->enabled = !!(ier & M5441X_RTC_ISR_ALM);
	alrm->pending = !!(isr & M5441X_RTC_ISR_ALM);

	return 0;
}

static int m5441x_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct m5441x_rtc *p = dev_get_drvdata(dev);
	unsigned long flags;
	u16 ier;

	spin_lock_irqsave(&p->lock, flags);
	m5441x_rtc_unlock(p);
	ier = rtc_rd(p, M5441X_RTC_IER);
	if (enabled) {
		/*
		 * The comparator latches ISR_ALM even while IER masks delivery,
		 * so a match from a previous arming (or one surviving a reboot)
		 * would fire the moment IER is unmasked. Clear it first.
		 */
		rtc_wr(p, M5441X_RTC_ISR, M5441X_RTC_ISR_ALM);
		ier |= M5441X_RTC_ISR_ALM;
	} else {
		ier &= ~M5441X_RTC_ISR_ALM;
	}
	rtc_wr(p, M5441X_RTC_IER, ier);
	spin_unlock_irqrestore(&p->lock, flags);

	return 0;
}

static int m5441x_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct m5441x_rtc *p = dev_get_drvdata(dev);
	s8 yoff = (alrm->time.tm_year + 1900) - M5441X_RTC_YEAR_BASE;
	unsigned long flags;
	u16 ier, cr;

	spin_lock_irqsave(&p->lock, flags);
	m5441x_rtc_unlock(p);
	/* acknowledge any pending alarm (write-1-to-clear) */
	rtc_wr(p, M5441X_RTC_ISR, M5441X_RTC_ISR_ALM);

	/*
	 * Match the full date so the alarm is a one-shot rather than the
	 * hardware default of a daily hour:minute:second match.
	 */
	cr = rtc_rd(p, M5441X_RTC_CR);
	m5441x_rtc_write_cr(p, (cr & ~M5441X_RTC_CR_AM_MASK) |
				M5441X_RTC_CR_AM_FULL);

	rtc_wr(p, M5441X_RTC_ALM_YRMON,
	       ((u16)((u8)yoff) << 8) | ((alrm->time.tm_mon + 1) & 0xff));
	/* RTC_ALM_DAYS has no day-of-week field; bits [15:8] are reserved. */
	rtc_wr(p, M5441X_RTC_ALM_DAYS, alrm->time.tm_mday & 0x1f);
	rtc_wr(p, M5441X_RTC_ALM_HM,
	       ((alrm->time.tm_hour & 0x1f) << 8) | (alrm->time.tm_min & 0x3f));
	rtc_wr(p, M5441X_RTC_ALM_SEC, alrm->time.tm_sec & 0x3f);

	ier = rtc_rd(p, M5441X_RTC_IER);
	if (alrm->enabled)
		ier |= M5441X_RTC_ISR_ALM;
	else
		ier &= ~M5441X_RTC_ISR_ALM;
	rtc_wr(p, M5441X_RTC_IER, ier);
	spin_unlock_irqrestore(&p->lock, flags);

	return 0;
}

static irqreturn_t m5441x_rtc_irq(int irq, void *dev_id)
{
	struct m5441x_rtc *p = dev_id;
	u16 status;

	spin_lock(&p->lock);
	/*
	 * Mask the reserved bit 0 (always reads set) so only a real source
	 * can claim the interrupt.
	 */
	status = rtc_rd(p, M5441X_RTC_ISR) & rtc_rd(p, M5441X_RTC_IER) &
		 ~M5441X_RTC_IER_RSVD;
	if (status) {
		m5441x_rtc_unlock(p);
		rtc_wr(p, M5441X_RTC_ISR, status);	/* write-1-to-clear */
	}
	spin_unlock(&p->lock);

	if (!status)
		return IRQ_NONE;

	if (status & M5441X_RTC_ISR_ALM)
		rtc_update_irq(p->rtc, 1, RTC_AF | RTC_IRQF);

	/*
	 * A hardware source was acknowledged above; claim the IRQ even when it
	 * was not the alarm so genirq's spurious detector does not disable the
	 * shared line.
	 */
	return IRQ_HANDLED;
}

static int m5441x_rtc_nvram_read(void *priv, unsigned int offset,
				 void *val, size_t bytes)
{
	struct m5441x_rtc *p = priv;
	u8 *buf = val;
	size_t done;

	/*
	 * In-kernel nvmem_device_read() forwards offset/bytes verbatim, so
	 * range-check here rather than trust the caller.
	 */
	if (offset >= M5441X_RTC_SRAM_SIZE ||
	    bytes > M5441X_RTC_SRAM_SIZE - offset)
		return -EINVAL;

	/*
	 * Process the transfer in chunks, releasing the lock between them, so
	 * a full 2KB access does not keep hard interrupts disabled across
	 * thousands of slow on-chip MMIO cycles and wreck IRQ latency.
	 */
	for (done = 0; done < bytes; done += M5441X_RTC_SRAM_CHUNK) {
		unsigned int chunk = min_t(unsigned int, bytes - done,
					   M5441X_RTC_SRAM_CHUNK);
		unsigned int i;

		scoped_guard(spinlock_irqsave, &p->lock)
			for (i = 0; i < chunk; i++)
				buf[done + i] = ioread8(p->base + M5441X_RTC_SRAM_OFFSET +
							offset + done + i);
	}

	return 0;
}

static int m5441x_rtc_nvram_write(void *priv, unsigned int offset,
				  void *val, size_t bytes)
{
	struct m5441x_rtc *p = priv;
	u8 *buf = val;
	size_t done;

	/* See the read path: validate the range, the core does not. */
	if (offset >= M5441X_RTC_SRAM_SIZE ||
	    bytes > M5441X_RTC_SRAM_SIZE - offset)
		return -EINVAL;

	/*
	 * Chunk the write and drop the lock between chunks (see the read
	 * path). The write window stays open for ~2 seconds, far longer than
	 * one chunk takes, so re-knock at the start of each chunk to keep it
	 * open across the lock-drop.
	 */
	for (done = 0; done < bytes; done += M5441X_RTC_SRAM_CHUNK) {
		unsigned int chunk = min_t(unsigned int, bytes - done,
					   M5441X_RTC_SRAM_CHUNK);
		unsigned int i;

		scoped_guard(spinlock_irqsave, &p->lock) {
			m5441x_rtc_knock(p);
			for (i = 0; i < chunk; i++)
				iowrite8(buf[done + i], p->base + M5441X_RTC_SRAM_OFFSET +
					 offset + done + i);
		}
	}

	return 0;
}

static const struct rtc_class_ops m5441x_rtc_ops = {
	.read_time		= m5441x_rtc_read_time,
	.set_time		= m5441x_rtc_set_time,
	.read_alarm		= m5441x_rtc_read_alarm,
	.set_alarm		= m5441x_rtc_set_alarm,
	.alarm_irq_enable	= m5441x_rtc_alarm_irq_enable,
};

static int m5441x_rtc_probe(struct platform_device *pdev)
{
	struct nvmem_config nvmem_cfg = {
		.name		= "m5441x_rtc_sram",
		.type		= NVMEM_TYPE_BATTERY_BACKED,
		.word_size	= 1,
		.stride		= 1,
		.size		= M5441X_RTC_SRAM_SIZE,
		.reg_read	= m5441x_rtc_nvram_read,
		.reg_write	= m5441x_rtc_nvram_write,
	};
	struct m5441x_rtc *p;
	unsigned long flags;
	bool osc_dead = false;
	int irq, ret;
	u16 cr, cfg;

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	spin_lock_init(&p->lock);

	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);

	platform_set_drvdata(pdev, p);

	/* Enable the oscillator if needed and mask/clear stale interrupts. */
	spin_lock_irqsave(&p->lock, flags);
	m5441x_rtc_unlock(p);
	/*
	 * Force binary, DST-free time/date encoding. The driver does not
	 * handle the BCD format, and Linux expects the RTC to keep monotonic
	 * time with local-time/DST conversion done in userspace, so clear
	 * BCDEN and DSTEN in case a bootloader or prior OS left them set. Also
	 * default the alarm match field to a full one-shot match: the reset
	 * value (AM=00) is a daily hh:mm:ss match, which would fire on the
	 * wrong day if userspace enables the alarm via RTC_AIE_ON without
	 * first programming it through set_alarm().
	 */
	cr = rtc_rd(p, M5441X_RTC_CR);
	cr &= ~(M5441X_RTC_CR_BCDEN | M5441X_RTC_CR_DSTEN | M5441X_RTC_CR_AM_MASK);
	cr |= M5441X_RTC_CR_AM_FULL;
	m5441x_rtc_write_cr(p, cr);

	/*
	 * The alarm registers are battery-backed and survive a power cycle,
	 * so a previously armed match could re-fire before set_alarm() runs.
	 * Clear them now (the write window is open).
	 */
	rtc_wr(p, M5441X_RTC_ALM_YRMON, 0);
	rtc_wr(p, M5441X_RTC_ALM_DAYS, 0);
	rtc_wr(p, M5441X_RTC_ALM_HM, 0);
	rtc_wr(p, M5441X_RTC_ALM_SEC, 0);

	/*
	 * Enable the 32kHz oscillator that drives the time counters. This only
	 * checks that the OSCEN enable bit latches: when it reads back clear
	 * the oscillator block rejected the enable, which in practice means no
	 * RTC crystal is fitted and the counters will never advance. It cannot
	 * tell a dead crystal from one still starting up, so it is a best-
	 * effort hint, not a liveness guarantee. Warn but keep probing, since
	 * the battery-backed standby RAM stays usable without the oscillator.
	 */
	cfg = rtc_rd(p, M5441X_RTC_CFG_DATA);
	if (!(cfg & M5441X_RTC_CFG_OSCEN)) {
		rtc_wr(p, M5441X_RTC_CFG_DATA, cfg | M5441X_RTC_CFG_OSCEN);
		if (!(rtc_rd(p, M5441X_RTC_CFG_DATA) & M5441X_RTC_CFG_OSCEN))
			osc_dead = true;
	}

	/* Mask every source but keep the reserved bit 0 set as required. */
	rtc_wr(p, M5441X_RTC_IER, M5441X_RTC_IER_RSVD);
	rtc_wr(p, M5441X_RTC_ISR, rtc_rd(p, M5441X_RTC_ISR));
	spin_unlock_irqrestore(&p->lock, flags);

	/*
	 * Keep the device (and its standby-RAM nvmem) usable, but record the
	 * dead oscillator so read_time/set_time refuse to expose a clock that
	 * cannot tick.
	 */
	p->osc_dead = osc_dead;
	if (osc_dead)
		dev_warn(&pdev->dev,
			 "RTC oscillator enable did not latch; timekeeping unavailable (no 32kHz crystal?)\n");

	p->rtc = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(p->rtc))
		return PTR_ERR(p->rtc);

	p->rtc->ops = &m5441x_rtc_ops;
	p->rtc->range_min = mktime64(1984, 1, 1, 0, 0, 0);
	p->rtc->range_max = mktime64(2239, 12, 31, 23, 59, 59);

	irq = platform_get_irq_optional(pdev, 0);
	if (irq > 0) {
		ret = devm_request_irq(&pdev->dev, irq, m5441x_rtc_irq, 0,
				       dev_name(&pdev->dev), p);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to request IRQ %d: %d, alarm disabled\n",
				irq, ret);
			clear_bit(RTC_FEATURE_ALARM, p->rtc->features);
		} else {
			/*
			 * The alarm comparator runs on standby power, so let it
			 * wake a suspended system. The PM core keeps the IRQ
			 * enabled across suspend once it is the device's wake
			 * IRQ; both are device-managed.
			 */
			devm_device_init_wakeup(&pdev->dev);
			devm_pm_set_wake_irq(&pdev->dev, irq);
		}
	} else {
		clear_bit(RTC_FEATURE_ALARM, p->rtc->features);
	}

	/*
	 * Register the RTC device before the nvmem provider:
	 * devm_rtc_nvmem_register() copies rtc->owner into the nvmem config,
	 * and that field is only assigned by devm_rtc_register_device().
	 * Registering nvmem first would leave its owner NULL and fail to pin
	 * this module while standby-RAM consumers hold references.
	 */
	ret = devm_rtc_register_device(p->rtc);
	if (ret)
		return ret;

	nvmem_cfg.priv = p;
	ret = devm_rtc_nvmem_register(p->rtc, &nvmem_cfg);
	if (ret)
		dev_warn(&pdev->dev, "standby RAM nvmem unavailable: %d\n", ret);

	return 0;
}

static struct platform_driver m5441x_rtc_driver = {
	.driver = {
		.name = "rtc-m5441x",
	},
	.probe = m5441x_rtc_probe,
};
module_platform_driver(m5441x_rtc_driver);

MODULE_DESCRIPTION("Freescale MCF5441x on-chip RTC driver");
MODULE_AUTHOR("Jean-Michel Hautbois <jeanmichel.hautbois@yoseli.org>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:rtc-m5441x");
