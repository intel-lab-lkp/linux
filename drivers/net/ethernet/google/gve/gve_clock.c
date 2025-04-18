// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/* Google virtual Ethernet (gve) driver
 *
 * Copyright (C) 2015-2025 Google LLC
 */

#include "gve.h"
#include "gve_adminq.h"

/* Interval to schedule a nic timestamp calibration, 250ms. */
#define GVE_NIC_TS_SYNC_INTERVAL_MS 250

/* Read the nic timestamp from hardware via the admin queue. */
static int gve_clock_nic_ts_read(struct gve_priv *priv)
{
	u64 nic_raw;
	int err;

	err = gve_adminq_report_nic_ts(priv, priv->nic_ts_report_bus);
	if (err)
		return err;

	nic_raw = be64_to_cpu(priv->nic_ts_report->nic_timestamp);
	WRITE_ONCE(priv->last_sync_nic_counter, nic_raw);

	return 0;
}

static void gve_nic_ts_sync_task(struct work_struct *work)
{
	struct gve_priv *priv = container_of(work, struct gve_priv,
					     nic_ts_sync_task.work);
	int err;

	if (gve_get_reset_in_progress(priv) || !gve_get_admin_queue_ok(priv))
		goto out;

	err = gve_clock_nic_ts_read(priv);
	if (err && net_ratelimit())
		dev_err(&priv->pdev->dev,
			"%s read err %d\n", __func__, err);

out:
	queue_delayed_work(priv->gve_wq, &priv->nic_ts_sync_task,
			   msecs_to_jiffies(GVE_NIC_TS_SYNC_INTERVAL_MS));
}

int gve_init_clock(struct gve_priv *priv)
{
	int err;

	if (!priv->nic_timestamp_supported)
		return -EPERM;

	priv->nic_ts_report =
		dma_alloc_coherent(&priv->pdev->dev,
				   sizeof(struct gve_nic_ts_report),
				   &priv->nic_ts_report_bus,
				   GFP_KERNEL);
	if (!priv->nic_ts_report) {
		dev_err(&priv->pdev->dev, "%s dma alloc error\n", __func__);
		return -ENOMEM;
	}

	err = gve_clock_nic_ts_read(priv);
	if (err) {
		dev_err(&priv->pdev->dev, "%s read error %d\n", __func__, err);
		goto free_nic_ts_report;
	}

	priv->gve_ts_wq = alloc_ordered_workqueue("gve-ts", 0);
	if (!priv->gve_ts_wq) {
		dev_err(&priv->pdev->dev, "%s Could not allocate workqueue\n",
			__func__);
		err = -ENOMEM;
		goto free_nic_ts_report;
	}
	INIT_DELAYED_WORK(&priv->nic_ts_sync_task, gve_nic_ts_sync_task);
	queue_delayed_work(priv->gve_ts_wq, &priv->nic_ts_sync_task,
			   msecs_to_jiffies(GVE_NIC_TS_SYNC_INTERVAL_MS));

	return 0;

free_nic_ts_report:
	dma_free_coherent(&priv->pdev->dev,
			  sizeof(struct gve_nic_ts_report),
			  priv->nic_ts_report, priv->nic_ts_report_bus);
	priv->nic_ts_report = NULL;

	return err;
}

void gve_teardown_clock(struct gve_priv *priv)
{
	if (priv->nic_ts_report) {
		cancel_delayed_work_sync(&priv->nic_ts_sync_task);
		destroy_workqueue(priv->gve_ts_wq);
		dma_free_coherent(&priv->pdev->dev,
				  sizeof(struct gve_nic_ts_report),
				  priv->nic_ts_report, priv->nic_ts_report_bus);
		priv->nic_ts_report = NULL;
	}
}
