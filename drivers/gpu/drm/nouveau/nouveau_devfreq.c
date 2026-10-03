// SPDX-License-Identifier: MIT
/*
 * Load-based performance level selection through devfreq.
 *
 * In automatic ("perfmon") mode, nvkm_pstate_work() programs the pstate held
 * in clk->astate.  This registers a devfreq device with one OPP per pstate,
 * keyed by its core clock, and lets the simple_ondemand governor move astate
 * according to the graphics engine load measured by the PMU.  A fixed pstate
 * requested through debugfs keeps taking precedence, as it does over astate
 * already.
 */
#include <linux/devfreq.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/units.h>

#include <nvif/if0001.h>

#include <subdev/clk.h>
#include <subdev/pmu.h>

#include "nouveau_devfreq.h"
#include "nouveau_drv.h"

/*
 * Each pstate change pauses PFIFO, waits for the engines to go idle and may
 * reclock memory, so sample at a moderate rate and keep the current pstate
 * while the load stays between UPTHRESHOLD - DOWNDIFFERENTIAL and UPTHRESHOLD.
 */
#define NOUVEAU_DEVFREQ_POLL_MS			100
#define NOUVEAU_DEVFREQ_UPTHRESHOLD		50
#define NOUVEAU_DEVFREQ_DOWNDIFFERENTIAL	20

struct nouveau_devfreq {
	struct devfreq *devfreq;
	struct devfreq_dev_profile profile;
	struct devfreq_simple_ondemand_data gov_data;

	/*
	 * Serialises the devfreq callbacks with suspend and resume, so that
	 * none of them touches the GPU while it may be powered down, and
	 * protects the members below.
	 */
	struct mutex lock;
	bool suspended;
	ktime_t last_sample;
	/* also read without the lock, see nouveau_devfreq_get_cur_freq() */
	unsigned long cur_freq;
};

static unsigned long
nouveau_devfreq_pstate_freq(const struct nvkm_pstate *pstate)
{
	return pstate->base.domain[nv_clk_src_core] * HZ_PER_KHZ;
}

/*
 * Return the position in clk->states of the pstate whose core clock is closest
 * to @freq, and store that core clock in @pfreq unless it is NULL.
 */
static int
nouveau_devfreq_pstate(struct nvkm_clk *clk, unsigned long freq,
		       unsigned long *pfreq)
{
	unsigned long best_diff = ULONG_MAX, best_freq = 0;
	struct nvkm_pstate *pstate;
	int i = 0, best = 0;

	list_for_each_entry(pstate, &clk->states, head) {
		unsigned long pstate_freq = nouveau_devfreq_pstate_freq(pstate);

		if (abs_diff(pstate_freq, freq) < best_diff) {
			best_diff = abs_diff(pstate_freq, freq);
			best_freq = pstate_freq;
			best = i;
		}
		i++;
	}

	if (pfreq)
		*pfreq = best_freq;
	return best;
}

/*
 * Read the core clock and cache the frequency of the pstate it belongs to.  The
 * PLLs do not always hit the vbios value exactly, and simple_ondemand rounds
 * its target up to the next OPP, so a raw reading slightly above an OPP would
 * make it step up.
 */
static int
nouveau_devfreq_update_freq(struct nouveau_devfreq *ndf, struct nvkm_clk *clk)
{
	unsigned long freq;
	int khz;

	khz = nvkm_clk_read(clk, nv_clk_src_core);
	if (khz < 0)
		return khz;

	nouveau_devfreq_pstate(clk, khz * HZ_PER_KHZ, &freq);
	WRITE_ONCE(ndf->cur_freq, freq);
	return 0;
}

static int
nouveau_devfreq_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct nouveau_drm *drm = dev_get_drvdata(dev);
	struct nouveau_devfreq *ndf = drm->devfreq;
	struct nvkm_clk *clk = nvxx_clk(drm);
	struct dev_pm_opp *opp;
	int ret = 0;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);

	mutex_lock(&ndf->lock);
	/* Nothing to do while suspended: resuming selects the highest pstate. */
	if (ndf->suspended) {
		*freq = ndf->cur_freq;
		goto out_unlock;
	}

	ret = nvkm_clk_astate(clk, nouveau_devfreq_pstate(clk, *freq, NULL), 0,
			      true);
	if (ret)
		goto out_unlock;

	/* Report the pstate in effect: a fixed one set through debugfs wins. */
	ret = nouveau_devfreq_update_freq(ndf, clk);
	if (!ret)
		*freq = ndf->cur_freq;

out_unlock:
	mutex_unlock(&ndf->lock);
	return ret;
}

static int
nouveau_devfreq_get_dev_status(struct device *dev,
			       struct devfreq_dev_status *stat)
{
	struct nouveau_drm *drm = dev_get_drvdata(dev);
	struct nouveau_devfreq *ndf = drm->devfreq;
	u32 busy, total;
	ktime_t now;
	int ret = 0;

	mutex_lock(&ndf->lock);
	now = ktime_get();
	stat->total_time = ktime_to_ns(ktime_sub(now, ndf->last_sample));
	stat->busy_time = 0;
	ndf->last_sample = now;

	if (ndf->suspended)
		goto out_unlock;

	ret = nvkm_pmu_perfmon_read(nvxx_device(drm)->pmu, &busy, &total);
	if (ret)
		goto out_unlock;

	ret = nouveau_devfreq_update_freq(ndf, nvxx_clk(drm));
	if (ret)
		goto out_unlock;

	/* Nothing counted, or a wrapped counter, gives no load: assume full. */
	if (total && busy <= total)
		stat->busy_time = mul_u64_u32_div(stat->total_time, busy, total);
	else
		stat->busy_time = stat->total_time;

out_unlock:
	stat->current_frequency = ndf->cur_freq;
	mutex_unlock(&ndf->lock);
	return ret;
}

/* The cur_freq sysfs attribute calls this without devfreq->lock. */
static int
nouveau_devfreq_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct nouveau_drm *drm = dev_get_drvdata(dev);

	*freq = READ_ONCE(drm->devfreq->cur_freq);
	return 0;
}

void
nouveau_devfreq_init(struct nouveau_drm *drm)
{
	struct nvkm_device *device = nvxx_device(drm);
	struct device *dev = drm->dev->dev;
	struct nvkm_clk *clk = device->clk;
	struct nouveau_devfreq *ndf;
	struct nvkm_pstate *pstate;
	unsigned long freq = 0;
	int ret;

	/*
	 * Only take over the automatic mode selected through NvClkMode.  Both
	 * NvClkMode=auto and the NVIF pstate method store this value for it.
	 */
	if (!clk || clk->state_nr < 2 ||
	    (clk->ustate_ac != NVIF_CONTROL_PSTATE_USER_V0_STATE_PERFMON &&
	     clk->ustate_dc != NVIF_CONTROL_PSTATE_USER_V0_STATE_PERFMON))
		return;

	/*
	 * Check for the PMU counters before adding OPPs: on Tegra the OPP table
	 * of this device belongs to gk20a_devfreq, and the error path below
	 * would remove its OPPs.
	 */
	if (nvkm_pmu_perfmon_init(device->pmu))
		return;

	/* OPPs are looked up by frequency, pstates by position in the list. */
	list_for_each_entry(pstate, &clk->states, head) {
		if (nouveau_devfreq_pstate_freq(pstate) <= freq) {
			NV_INFO(drm, "devfreq: pstate core clocks not increasing\n");
			goto err_opp;
		}
		freq = nouveau_devfreq_pstate_freq(pstate);

		ret = dev_pm_opp_add(dev, freq, 0);
		if (ret) {
			NV_ERROR(drm, "devfreq: failed to add OPP, %d\n", ret);
			goto err_opp;
		}
	}

	ndf = kzalloc_obj(*ndf);
	if (!ndf)
		goto err_opp;

	mutex_init(&ndf->lock);
	ret = nouveau_devfreq_update_freq(ndf, clk);
	if (ret)
		goto err_free;
	ndf->last_sample = ktime_get();

	/* A deferrable timer would let the 31-bit PMU counters wrap when idle. */
	ndf->profile.timer = DEVFREQ_TIMER_DELAYED;
	ndf->profile.polling_ms = NOUVEAU_DEVFREQ_POLL_MS;
	ndf->profile.initial_freq = ndf->cur_freq;
	ndf->profile.target = nouveau_devfreq_target;
	ndf->profile.get_dev_status = nouveau_devfreq_get_dev_status;
	ndf->profile.get_cur_freq = nouveau_devfreq_get_cur_freq;
	ndf->gov_data.upthreshold = NOUVEAU_DEVFREQ_UPTHRESHOLD;
	ndf->gov_data.downdifferential = NOUVEAU_DEVFREQ_DOWNDIFFERENTIAL;

	drm->devfreq = ndf;
	ndf->devfreq = devfreq_add_device(dev, &ndf->profile,
					  DEVFREQ_GOV_SIMPLE_ONDEMAND,
					  &ndf->gov_data);
	if (IS_ERR(ndf->devfreq)) {
		NV_ERROR(drm, "devfreq: failed to register, %ld\n",
			 PTR_ERR(ndf->devfreq));
		drm->devfreq = NULL;
		goto err_free;
	}

	return;

err_free:
	mutex_destroy(&ndf->lock);
	kfree(ndf);
err_opp:
	dev_pm_opp_remove_all_dynamic(dev);
}

void
nouveau_devfreq_fini(struct nouveau_drm *drm)
{
	struct nouveau_devfreq *ndf = drm->devfreq;

	if (!ndf)
		return;

	devfreq_remove_device(ndf->devfreq);
	dev_pm_opp_remove_all_dynamic(drm->dev->dev);
	drm->devfreq = NULL;
	mutex_destroy(&ndf->lock);
	kfree(ndf);
}

void
nouveau_devfreq_suspend(struct nouveau_drm *drm)
{
	struct nouveau_devfreq *ndf = drm->devfreq;

	if (!ndf)
		return;

	/*
	 * devfreq_suspend_device() only stops the monitor; sysfs and PM QoS
	 * requests still reach the callbacks, which check ->suspended.
	 */
	mutex_lock(&ndf->lock);
	ndf->suspended = true;
	mutex_unlock(&ndf->lock);

	devfreq_suspend_device(ndf->devfreq);
}

void
nouveau_devfreq_resume(struct nouveau_drm *drm)
{
	struct nouveau_devfreq *ndf = drm->devfreq;

	if (!ndf)
		return;

	mutex_lock(&ndf->lock);
	/* Resuming the PMU reset its counters, and nvkm_clk_init() the pstate. */
	nvkm_pmu_perfmon_init(nvxx_device(drm)->pmu);
	nouveau_devfreq_update_freq(ndf, nvxx_clk(drm));
	ndf->last_sample = ktime_get();
	ndf->suspended = false;
	mutex_unlock(&ndf->lock);

	devfreq_resume_device(ndf->devfreq);
}
