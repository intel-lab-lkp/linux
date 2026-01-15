// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/* Google virtual Ethernet (gve) driver
 *
 * Copyright (C) 2025 Google LLC
 */

#include "gve.h"
#include "gve_adminq.h"

/* Interval to schedule a nic timestamp calibration, 250ms. */
#define GVE_NIC_TS_SYNC_INTERVAL_MS 250

/* Scale ts_real.cc.mult by 1 << 31. Maximize mult for finer adjustment
 * granularity, but ensure (mult * cycle) does not overflow in
 * cyclecounter_cyc2ns.
 */
#define GVE_HWTS_REAL_CC_SHIFT 31
#define GVE_HWTS_REAL_CC_NOMINAL BIT_ULL(GVE_HWTS_REAL_CC_SHIFT)

/* Get the cross time stamp info */
static int gve_get_cross_time(ktime_t *device,
			      struct system_counterval_t *system, void *ctx)
{
	struct gve_priv *priv = ctx;

	*device = ns_to_ktime(be64_to_cpu(priv->nic_ts_report->nic_timestamp));
	system->cycles = be64_to_cpu(priv->nic_ts_report->cycle_pre) +
			 (be64_to_cpu(priv->nic_ts_report->cycle_post) -
			  be64_to_cpu(priv->nic_ts_report->cycle_pre)) / 2;
	system->use_nsecs = false;
	if (IS_ENABLED(CONFIG_X86))
		system->cs_id = CSID_X86_TSC;
	else if (IS_ENABLED(CONFIG_ARM_ARCH_TIMER))
		system->cs_id = CSID_ARM_ARCH_COUNTER;
	else
		return -EOPNOTSUPP;

	return 0;
}

static int gve_hwts_realtime_update(struct gve_priv *priv, u64 prev_nic)
{
	struct system_device_crosststamp cts = {};
	struct system_time_snapshot history = {};
	s64 nic_real_off_ns;
	u64 real_ns;
	int ret;

	/* Step 1: Get the realtime of when NIC clock was read */
	ktime_get_snapshot(&history);
	ret = get_device_system_crosststamp(gve_get_cross_time, priv, &history,
					    &cts);
	if (ret) {
		dev_err_ratelimited(&priv->pdev->dev,
				    "%s crosststamp err %d\n", __func__, ret);
		return ret;
	}

	real_ns = ktime_to_ns(cts.sys_realtime);

	/* Step 2: Adjust NIC clock's offset */
	/* Read-side ndo_get_tstamp can be called from TCP rx softirq */
	write_seqlock_bh(&priv->ts_real.lock);
	nic_real_off_ns = real_ns - timecounter_read(&priv->ts_real.tc);
	timecounter_adjtime(&priv->ts_real.tc, nic_real_off_ns);

	/* Step 3: Adjust NIC clock's ratio (when this is not the first sync).
	 * The NIC clock's nominal tick ratio is 1 tick per nanosecond,
	 * scaled by 1 << GVE_HWTS_REAL_CC_SHIFT. Adjust it to
	 * (ktime - prev_ktime) / (nic - prev_nic). The ratio should not
	 * deviate more than 1% from the nominal, otherwise it may suggest
	 * there was a sudden change on NIC clock. In that case, reset ratio
	 * to nominal. And since each sync only compares to the previous read,
	 * this is a one-time error, not a persistent failure.
	 */
	if (prev_nic) {
		const u64 lower = GVE_HWTS_REAL_CC_NOMINAL * 99 / 100;
		const u64 upper = GVE_HWTS_REAL_CC_NOMINAL * 101 / 100;
		u64 mult;

		mult = mult_frac(GVE_HWTS_REAL_CC_NOMINAL,
				 real_ns - priv->ts_real.last_sync_ns,
				 priv->last_sync_nic_counter - prev_nic);
		if (mult < lower || mult > upper)
			mult = GVE_HWTS_REAL_CC_NOMINAL;
		priv->ts_real.cc.mult = mult;
	}

	write_sequnlock_bh(&priv->ts_real.lock);
	WRITE_ONCE(priv->ts_real.last_sync_ns, real_ns);
	return 0;
}

/* Read the nic timestamp from hardware via the admin queue. */
int gve_clock_nic_ts_read(struct gve_priv *priv)
{
	u64 nic_raw, prev_nic;
	int err;

	err = gve_adminq_report_nic_ts(priv, priv->nic_ts_report_bus);
	if (err)
		return err;

	nic_raw = be64_to_cpu(priv->nic_ts_report->nic_timestamp);
	prev_nic = priv->last_sync_nic_counter;
	WRITE_ONCE(priv->last_sync_nic_counter, nic_raw);
	err = gve_hwts_realtime_update(priv, prev_nic);
	if (err)
		return err;

	return 0;
}

static int gve_ptp_gettimex64(struct ptp_clock_info *info,
			      struct timespec64 *ts,
			      struct ptp_system_timestamp *sts)
{
	return -EOPNOTSUPP;
}

static int gve_ptp_settime64(struct ptp_clock_info *info,
			     const struct timespec64 *ts)
{
	return -EOPNOTSUPP;
}

static long gve_ptp_do_aux_work(struct ptp_clock_info *info)
{
	const struct gve_ptp *ptp = container_of(info, struct gve_ptp, info);
	struct gve_priv *priv = ptp->priv;
	int err;

	if (gve_get_reset_in_progress(priv) || !gve_get_admin_queue_ok(priv))
		goto out;

	err = gve_clock_nic_ts_read(priv);
	if (err && net_ratelimit())
		dev_err(&priv->pdev->dev,
			"%s read err %d\n", __func__, err);

out:
	return msecs_to_jiffies(GVE_NIC_TS_SYNC_INTERVAL_MS);
}

static u64 gve_cycles_read(struct cyclecounter *cc)
{
	const struct gve_priv *priv = container_of(cc, struct gve_priv,
						   ts_real.cc);

	return READ_ONCE(priv->last_sync_nic_counter);
}

static const struct ptp_clock_info gve_ptp_caps = {
	.owner          = THIS_MODULE,
	.name		= "gve clock",
	.gettimex64	= gve_ptp_gettimex64,
	.settime64	= gve_ptp_settime64,
	.do_aux_work	= gve_ptp_do_aux_work,
};

static int gve_ptp_init(struct gve_priv *priv)
{
	struct gve_ptp *ptp;
	int err;

	if (!priv->nic_timestamp_supported) {
		dev_dbg(&priv->pdev->dev, "Device does not support PTP\n");
		return -EOPNOTSUPP;
	}

	priv->ptp = kzalloc(sizeof(*priv->ptp), GFP_KERNEL);
	if (!priv->ptp)
		return -ENOMEM;

	ptp = priv->ptp;
	ptp->info = gve_ptp_caps;
	ptp->clock = ptp_clock_register(&ptp->info, &priv->pdev->dev);

	if (IS_ERR(ptp->clock)) {
		dev_err(&priv->pdev->dev, "PTP clock registration failed\n");
		err  = PTR_ERR(ptp->clock);
		goto free_ptp;
	}

	priv->last_sync_nic_counter = 0;
	priv->ts_real.last_sync_ns = 0;
	seqlock_init(&priv->ts_real.lock);
	memset(&priv->ts_real.cc, 0, sizeof(priv->ts_real.cc));
	priv->ts_real.cc.mask = U32_MAX;
	priv->ts_real.cc.shift = GVE_HWTS_REAL_CC_SHIFT;
	priv->ts_real.cc.mult = GVE_HWTS_REAL_CC_NOMINAL;
	priv->ts_real.cc.read = gve_cycles_read;
	timecounter_init(&priv->ts_real.tc, &priv->ts_real.cc,
			 ktime_get_real_ns());

	ptp->priv = priv;
	return 0;

free_ptp:
	kfree(ptp);
	priv->ptp = NULL;
	return err;
}

static void gve_ptp_release(struct gve_priv *priv)
{
	struct gve_ptp *ptp = priv->ptp;

	if (!ptp)
		return;

	if (ptp->clock)
		ptp_clock_unregister(ptp->clock);

	kfree(ptp);
	priv->ptp = NULL;
}

int gve_init_clock(struct gve_priv *priv)
{
	int err;

	if (!priv->nic_timestamp_supported)
		return 0;

	err = gve_ptp_init(priv);
	if (err)
		return err;

	priv->nic_ts_report =
		dma_alloc_coherent(&priv->pdev->dev,
				   sizeof(struct gve_nic_ts_report),
				   &priv->nic_ts_report_bus,
				   GFP_KERNEL);
	if (!priv->nic_ts_report) {
		dev_err(&priv->pdev->dev, "%s dma alloc error\n", __func__);
		err = -ENOMEM;
		goto release_ptp;
	}
	err = gve_clock_nic_ts_read(priv);
	if (err) {
		dev_err(&priv->pdev->dev, "failed to read NIC clock %d\n", err);
		goto release_nic_ts_report;
	}
	ptp_schedule_worker(priv->ptp->clock,
			    msecs_to_jiffies(GVE_NIC_TS_SYNC_INTERVAL_MS));

	return 0;

release_nic_ts_report:
	dma_free_coherent(&priv->pdev->dev,
			  sizeof(struct gve_nic_ts_report),
			  priv->nic_ts_report, priv->nic_ts_report_bus);
	priv->nic_ts_report = NULL;
release_ptp:
	gve_ptp_release(priv);
	return err;
}

void gve_teardown_clock(struct gve_priv *priv)
{
	gve_ptp_release(priv);

	if (priv->nic_ts_report) {
		dma_free_coherent(&priv->pdev->dev,
				  sizeof(struct gve_nic_ts_report),
				  priv->nic_ts_report, priv->nic_ts_report_bus);
		priv->nic_ts_report = NULL;
	}
}
