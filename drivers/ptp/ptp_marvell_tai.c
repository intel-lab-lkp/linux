// SPDX-License-Identifier: GPL-2.0+
/*
 * TAI (time application interface) driver for Marvell PHYs and Marvell NETA.
 *
 * This file implements TAI support as a PTP clock. Timecounter/cyclecounter
 * representation taken from Marvell 88E6xxx DSA driver. We may need to share
 * the TAI between multiple PHYs in a multiport PHY.
 */
#include <linux/if_ether.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/marvell_ptp.h>

#define TAI_CONFIG_0				0
#define TAI_CONFIG_0_EVENTCAPOV			BIT(15)
#define TAI_CONFIG_0_EVENTCTRSTART		BIT(14)
#define TAI_CONFIG_0_EVENTPHASE			BIT(13)
#define TAI_CONFIG_0_TRIGGENINTEN		BIT(9)
#define TAI_CONFIG_0_EVENTCAPINTEN		BIT(8)

#define TAI_CONFIG_9				9
#define TAI_CONFIG_9_EVENTCAPERR		BIT(9)
#define TAI_CONFIG_9_EVENTCAPVALID		BIT(8)

#define TAI_EVENT_CAPTURE_TIME_LO		10
#define TAI_EVENT_CAPTURE_TIME_HI		11

#define PTPG_CONFIG_0				0
#define PTPG_CONFIG_1				1
#define PTPG_CONFIG_2				2
#define PTPG_CONFIG_3				3
#define PTPG_CONFIG_3_TSATSFD			BIT(0)
#define PTPG_STATUS				8

#define TAI_EVENT_POLL_INTERVAL msecs_to_jiffies(100)

struct marvell_tai {
	const struct marvell_ptp_ops *ops;
	struct device *dev;

	struct ptp_clock_info caps;
	struct ptp_clock *ptp_clock;

	u32 cc_mult_num;
	u32 cc_mult_den;
	u32 cc_mult;

	struct mutex mutex;
	struct timecounter timecounter;
	struct cyclecounter cyclecounter;

	long half_overflow_period;
	struct delayed_work overflow_work;

	bool defunct;
	bool extts_poll;
	struct delayed_work event_work;

	/* Used while reading the TAI */
	struct ptp_system_timestamp *sts;
};

static struct marvell_tai *cc_to_tai(const struct cyclecounter *cc)
{
	return container_of(cc, struct marvell_tai, cyclecounter);
}

/* Read the global time registers using the readplus command */
static u64 marvell_tai_clock_read(const struct cyclecounter *cc)
{
	struct marvell_tai *tai = cc_to_tai(cc);

	return tai->ops->tai_clock_read(tai->dev, tai->sts);
}

u64 marvell_tai_cyc2time(struct marvell_tai *tai, u32 cyc)
{
	u64 ns;

	mutex_lock(&tai->mutex);
	ns = timecounter_cyc2time(&tai->timecounter, cyc);
	mutex_unlock(&tai->mutex);

	return ns;
}
EXPORT_SYMBOL_GPL(marvell_tai_cyc2time);

static struct marvell_tai *ptp_to_tai(struct ptp_clock_info *ptp)
{
	return container_of(ptp, struct marvell_tai, caps);
}

static int marvell_tai_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct marvell_tai *tai = ptp_to_tai(ptp);
	bool neg;
	u32 diff;
	u64 adj;

	neg = scaled_ppm < 0;
	if (neg)
		scaled_ppm = -scaled_ppm;

	adj = tai->cc_mult_num;
	adj *= scaled_ppm;
	diff = div_u64(adj, tai->cc_mult_den);

	mutex_lock(&tai->mutex);
	timecounter_read(&tai->timecounter);
	tai->cyclecounter.mult = neg ? tai->cc_mult - diff :
				       tai->cc_mult + diff;
	mutex_unlock(&tai->mutex);

	return 0;
}

static int marvell_tai_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct marvell_tai *tai = ptp_to_tai(ptp);

	mutex_lock(&tai->mutex);
	timecounter_adjtime(&tai->timecounter, delta);
	mutex_unlock(&tai->mutex);

	return 0;
}

static int marvell_tai_gettimex64(struct ptp_clock_info *ptp,
				  struct timespec64 *ts,
				  struct ptp_system_timestamp *sts)
{
	struct marvell_tai *tai = ptp_to_tai(ptp);
	u64 ns;

	mutex_lock(&tai->mutex);
	tai->sts = sts;
	ns = timecounter_read(&tai->timecounter);
	tai->sts = NULL;
	mutex_unlock(&tai->mutex);

	*ts = ns_to_timespec64(ns);

	return 0;
}

static int marvell_tai_settime64(struct ptp_clock_info *ptp,
				 const struct timespec64 *ts)
{
	struct marvell_tai *tai = ptp_to_tai(ptp);
	u64 ns = timespec64_to_ns(ts);

	mutex_lock(&tai->mutex);
	timecounter_init(&tai->timecounter, &tai->cyclecounter, ns);
	mutex_unlock(&tai->mutex);

	return 0;
}

static void marvell_tai_extts(struct marvell_tai *tai)
{
	struct marvell_extts extts;
	struct ptp_clock_event ev;
	int err;

	err = tai->ops->tai_extts_read(tai->dev, TAI_CONFIG_9, &extts);
	if (err <= 0)
		return;

	if (extts.status & TAI_CONFIG_9_EVENTCAPERR) {
		dev_warn(tai->dev, "extts timestamp overrun (%x)\n",
			 extts.status);
		return;
	}

	ev.type = PTP_CLOCK_EXTTS;
	ev.index = 0;
	ev.timestamp = marvell_tai_cyc2time(tai, extts.time);

	ptp_clock_event(tai->ptp_clock, &ev);
}

static int marvell_tai_enable_extts(struct marvell_tai *tai,
				    struct ptp_extts_request *req, int enable)
{
	int err, pin;
	u16 cfg0;

	if (req->flags & ~(PTP_ENABLE_FEATURE | PTP_RISING_EDGE |
			   PTP_FALLING_EDGE | PTP_STRICT_FLAGS))
		return -EINVAL;

	pin = ptp_find_pin(tai->ptp_clock, PTP_PF_EXTTS, req->index);
	if (pin < 0)
		return -EBUSY;

	/* Setup this pin, validating flags as appropriate */
	err = tai->ops->tai_pin_setup(tai->dev, pin, req->flags, enable);
	if (err < 0)
		return err;

	if (enable) {
		/* Clear the status */
		err = tai->ops->tai_write(tai->dev, TAI_CONFIG_9, 0);
		if (err < 0)
			return err;

		cfg0 = TAI_CONFIG_0_EVENTCAPINTEN |
		       TAI_CONFIG_0_EVENTCTRSTART;

		/*
		 * For compatibility with DSA, we test for !rising rather
		 * than for falling.
		 */
		if (!(req->flags & PTP_RISING_EDGE))
			cfg0 |= TAI_CONFIG_0_EVENTPHASE;

		/* Enable the event interrupt and counter */
		err = tai->ops->tai_modify(tai->dev, TAI_CONFIG_0,
					   TAI_CONFIG_0_EVENTCAPOV |
					   TAI_CONFIG_0_EVENTCTRSTART |
					   TAI_CONFIG_0_EVENTCAPINTEN, cfg0);
		if (err < 0)
			return err;

		schedule_delayed_work(&tai->event_work,
				      TAI_EVENT_POLL_INTERVAL);
	} else {
		/* Disable the event interrupt and counter */
		err = tai->ops->tai_modify(tai->dev, TAI_CONFIG_0,
					   TAI_CONFIG_0_EVENTCTRSTART |
					   TAI_CONFIG_0_EVENTCAPINTEN, 0);
		if (err < 0)
			return err;

		cancel_delayed_work_sync(&tai->event_work);
	}

	return 0;
}

static int marvell_tai_enable(struct ptp_clock_info *ptp,
			      struct ptp_clock_request *req, int enable)
{
	struct marvell_tai *tai = ptp_to_tai(ptp);
	int err;

	switch (req->type) {
	case PTP_PF_EXTTS:
		err = marvell_tai_enable_extts(tai, &req->extts, enable);
		break;

	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

static int marvell_tai_verify(struct ptp_clock_info *ptp, unsigned int pin,
			      enum ptp_pin_function func, unsigned int chan)
{
	struct marvell_tai *tai = ptp_to_tai(ptp);

	/* Always allow a pin to be set to no function */
	if (func == PTP_PF_NONE)
		return 0;

	if (!tai->ops->tai_pin_verify)
		return -EOPNOTSUPP;

	return tai->ops->tai_pin_verify(tai->dev, pin, func, chan);
}

/* Periodically read the timecounter to keep the time refreshed. */
static long marvell_tai_aux_work(struct ptp_clock_info *ptp)
{
	struct marvell_tai *tai = ptp_to_tai(ptp);
	long ret = -1;

	if (tai->ops->ptp_aux_work)
		ret = tai->ops->ptp_aux_work(tai->dev);

	return ret;
}

#define event_work_to_tai(w) \
	container_of(to_delayed_work(w), struct marvell_tai, event_work)
static void marvell_tai_event_work(struct work_struct *w)
{
	struct marvell_tai *tai = event_work_to_tai(w);

	if (tai->defunct)
		return;

	marvell_tai_extts(tai);

	schedule_delayed_work(&tai->event_work, TAI_EVENT_POLL_INTERVAL);
}

#define overflow_work_to_tai(w) \
	container_of(to_delayed_work(w), struct marvell_tai, overflow_work)
static void marvell_tai_overflow_work(struct work_struct *w)
{
	struct marvell_tai *tai = overflow_work_to_tai(w);

	/* Read the timecounter to update */
	mutex_lock(&tai->mutex);
	timecounter_read(&tai->timecounter);
	mutex_unlock(&tai->mutex);

	schedule_delayed_work(&tai->overflow_work, tai->half_overflow_period);
}

/* Configure the global (shared between ports) configuration for the PHY. */
static int marvell_tai_global_config(struct marvell_tai *tai)
{
	int err;

	/* Enable TAI */
	err = tai->ops->tai_enable(tai->dev);
	if (err)
		return err;

	/* Set ether-type for IEEE1588 packets */
	err = tai->ops->ptp_global_write(tai->dev, PTPG_CONFIG_0, ETH_P_1588);
	if (err < 0)
		return err;

	/* MsdIDTSEn - Enable timestamping on all PTP MessageIDs */
	err = tai->ops->ptp_global_write(tai->dev, PTPG_CONFIG_1,
					 MV_PTP_MSD_ID_TS_EN);
	if (err < 0)
		return err;

	/* TSArrPtr - Point to Arr0 registers */
	err = tai->ops->ptp_global_write(tai->dev, PTPG_CONFIG_2,
					 MV_PTP_TS_ARR_PTR);
	if (err < 0)
		return err;

	/* TSAtSFD - timestamp at SFD */
	err = tai->ops->ptp_global_write(tai->dev, PTPG_CONFIG_3,
					 PTPG_CONFIG_3_TSATSFD);
	if (err < 0)
		return err;

	return 0;
}

int marvell_tai_ptp_clock_index(struct marvell_tai *tai)
{
	return ptp_clock_index(tai->ptp_clock);
}
EXPORT_SYMBOL_GPL(marvell_tai_ptp_clock_index);

int marvell_tai_schedule(struct marvell_tai *tai, unsigned long delay)
{
	return ptp_schedule_worker(tai->ptp_clock, delay);
}
EXPORT_SYMBOL_GPL(marvell_tai_schedule);

int marvell_tai_probe(struct marvell_tai **taip,
		      const struct marvell_ptp_ops *ops,
		      const struct marvell_tai_param *param,
		      struct ptp_pin_desc *pin_config, int n_pins,
		      const char *name, struct device *dev)
{
	struct marvell_tai *tai;
	u64 overflow_ns;
	int err;

	tai = devm_kzalloc(dev, sizeof(*tai), GFP_KERNEL);
	if (!tai)
		return -ENOMEM;

	mutex_init(&tai->mutex);

	tai->dev = dev;
	tai->ops = ops;
	tai->cc_mult_num = param->cc_mult_num;
	tai->cc_mult_den = param->cc_mult_den;
	tai->cc_mult = param->cc_mult;

	err = marvell_tai_global_config(tai);
	if (err < 0)
		return err;

	tai->cyclecounter.read = marvell_tai_clock_read;
	tai->cyclecounter.mask = CYCLECOUNTER_MASK(32);
	tai->cyclecounter.mult = param->cc_mult;
	tai->cyclecounter.shift = param->cc_shift;

	overflow_ns = BIT_ULL(32) * param->cc_mult;
	overflow_ns >>= param->cc_shift;
	tai->half_overflow_period = nsecs_to_jiffies64(overflow_ns / 2);

	timecounter_init(&tai->timecounter, &tai->cyclecounter,
			 ktime_to_ns(ktime_get_real()));

	tai->caps.owner = THIS_MODULE;
	strscpy(tai->caps.name, name, sizeof(tai->caps.name));
	/* max_adj of 1000000 is what MV88E6xxx DSA uses */
	tai->caps.max_adj = 1000000;
	tai->caps.n_ext_ts = param->n_ext_ts;
	tai->caps.n_pins = n_pins;
	tai->caps.pin_config = pin_config;
	tai->caps.adjfine = marvell_tai_adjfine;
	tai->caps.adjtime = marvell_tai_adjtime;
	tai->caps.gettimex64 = marvell_tai_gettimex64;
	tai->caps.settime64 = marvell_tai_settime64;
	tai->caps.enable = marvell_tai_enable;
	tai->caps.verify = marvell_tai_verify;
	tai->caps.do_aux_work = marvell_tai_aux_work;

	INIT_DELAYED_WORK(&tai->overflow_work, marvell_tai_overflow_work);
	INIT_DELAYED_WORK(&tai->event_work, marvell_tai_event_work);

	tai->ptp_clock = ptp_clock_register(&tai->caps, dev);
	if (IS_ERR(tai->ptp_clock)) {
		kfree(tai);
		return PTR_ERR(tai->ptp_clock);
	}

	/*
	 * Kick off the auxiliary worker to run once every half-overflow
	 * period to keep the timecounter properly updated.
	 */
	schedule_delayed_work(&tai->overflow_work, tai->half_overflow_period);

	*taip = tai;

	return 0;
}
EXPORT_SYMBOL_GPL(marvell_tai_probe);

void marvell_tai_remove(struct marvell_tai *tai)
{
	/* Avoid races with the event work - mark defunct before
	 * unregistering, which goes against "unpublish then tear down"
	 */
	tai->defunct = true;
	cancel_delayed_work_sync(&tai->event_work);

	ptp_clock_unregister(tai->ptp_clock);

	cancel_delayed_work_sync(&tai->overflow_work);
}
EXPORT_SYMBOL_GPL(marvell_tai_remove);
