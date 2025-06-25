// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PTP clock for Alibaba CIPU
 *
 * Copyright (C) 2025 Alibaba Inc.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/utsname.h>

#define DRV_VER_MAJOR		1
#define DRV_VER_MINOR		3
#define DRV_VER_SUBMINOR	0
#ifndef DRV_TYPE
#define DRV_TYPE		0
#endif
#define LINUX_UPSTREAM		0x1F

enum {
	PTP_CIPU_DEV_RESET,
	PTP_CIPU_DEV_INIT,
	PTP_CIPU_DEV_READY,
}; /* device status */

/* feature bits mask */
#define PTP_CIPU_M_FT_TAI	BIT(0) /* 1: support TAI. 0: def UTC */
#define PTP_CIPU_M_FT_EPOCH	BIT(1) /* 1: epoch base, 0: 1970-01-01. */
#define PTP_CIPU_M_FT_IRQ_ABN	BIT(2) /* 1: abnormal event irq. */
#define PTP_CIPU_M_FT_IRQ_REC	BIT(3) /* 1: recovery event irq. */

/* timestamp bits mask */
#define PTP_CIPU_M_TS_ABN	BIT_ULL(63) /* 1: normal, 0: abnormal */
#define PTP_CIPU_M_TS_RESVD	BIT_ULL(62) /* reserved now */

/* sync status bits mask */
#define PTP_CIPU_M_ST_DEV_MT	BIT(0) /* maintenance timeout */
#define PTP_CIPU_M_ST_CLK_ABN	BIT(1) /* atomic clock abnormal */
#define PTP_CIPU_M_ST_DEV_BUSY	BIT(4) /* cipu device busy */
#define PTP_CIPU_M_ST_DEV_ERR	BIT(7) /* cipu device error */

#define PTP_CIPU_M_ST_PHC_ISSUE	\
	(PTP_CIPU_M_ST_DEV_MT | PTP_CIPU_M_ST_CLK_ABN | \
	 PTP_CIPU_M_ST_DEV_BUSY | PTP_CIPU_M_ST_DEV_ERR)

/* TAI is not supported now */
#define PTP_CIPU_DRV_CAP (PTP_CIPU_M_FT_EPOCH | PTP_CIPU_M_FT_IRQ_ABN | \
			  PTP_CIPU_M_FT_IRQ_REC)

struct ptp_cipu_regs {
	u32	dev_feat;	/* RO, device features */
	u32	gst_feat;	/* RW, guest features */
	u32	drv_ver;	/* RW, driver version */
	u32	env_ver;	/* RW, environment version */
	u8	dev_stat;	/* RW, device status */
	u8	sync_stat;	/* RO, sync status */
	u16	reserved;
	u32	tm_prec_ns;	/* RO, time precision */
	u32	epo_base_yr;	/* RO, epoch base */
	u32	leap_sec;	/* RO, leap second */
	u32	max_lat_ns;	/* RO, max latency */
	u32	mt_tout_us;	/* RO, maintenance timeout */
	u64	tstamp_ns;	/* RO, timestamp */
	u32	thresh_us;	/* RO, threshold */
};

#define PTP_CIPU_BAR_0	0
#define PTP_CIPU_REG(reg) \
	offsetof(struct ptp_cipu_regs, reg)

enum {
	PTP_CIPU_EVT_TYPE_PTP,
	PTP_CIPU_EVT_TYPE_DEV,
	PTP_CIPU_EVT_TYPE_DRV,
}; /* event types */

enum {
	PTP_CIPU_EVT_P_GT_OPS,
	PTP_CIPU_EVT_P_GT_INVAL,
	PTP_CIPU_EVT_P_GT_TOUT,
	PTP_CIPU_EVT_P_GT_ETHRESH,
	PTP_CIPU_EVT_P_MAX,
}; /* events related to ptp operations */

enum {
	PTP_CIPU_EVT_H_CLK_ABN,
	PTP_CIPU_EVT_H_CLK_ABN_REC,
	PTP_CIPU_EVT_H_DEV_MT,
	PTP_CIPU_EVT_H_DEV_MT_REC,
	PTP_CIPU_EVT_H_DEV_MT_TOUT,
	PTP_CIPU_EVT_H_DEV_BUSY,
	PTP_CIPU_EVT_H_DEV_BUSY_REC,
	PTP_CIPU_EVT_H_DEV_ERR,
	PTP_CIPU_EVT_H_DEV_ERR_REC,
	PTP_CIPU_EVT_H_MAX,
}; /* events related to hardware device */

enum {
	PTP_CIPU_EVT_D_GENERAL,
	PTP_CIPU_EVT_D_PROBE_FAIL,
	PTP_CIPU_EVT_D_MAX,
}; /* events related to driver ifself */

struct ptp_cipu_stats {
	u64	ptp_evts[PTP_CIPU_EVT_P_MAX];
	u64	dev_evts[PTP_CIPU_EVT_H_MAX];
}; /* statistics */

#define PTP_CIPU_LOG_SUB(dev, level, type, event, fmt, ...) \
({ \
	static DEFINE_RATELIMIT_STATE(_rs, \
				      DEFAULT_RATELIMIT_INTERVAL, \
				      DEFAULT_RATELIMIT_BURST); \
	if (__ratelimit(&_rs)) \
		dev_printk(level, dev, "[%02x:%02x]: " fmt, \
			   type, event, ##__VA_ARGS__); \
})

#define PTP_CIPU_LOG(dev, level, event, fmt, ...) \
	PTP_CIPU_LOG_SUB(dev, level, PTP_CIPU_EVT_TYPE_DRV, \
			 event, fmt, ##__VA_ARGS__)

#define PTP_CIPU_TIMER_PERIOD	(30 * HZ)

struct ptp_cipu_timer_ctx {
	u64 over_thresh_cnt;
	u32 thresh_us;
};

enum {
	PTP_CIPU_IRQ_0,
	PTP_CIPU_IRQ_1,
	PTP_CIPU_IRQ_NUM,
};

struct ptp_cipu_irq_ctx {
	irqreturn_t (*irq_func)(int irq, void *data);
};

struct ptp_cipu_ctx {
	struct pci_dev *pdev;
	struct ptp_clock *ptp_clock;
	struct ptp_clock_info ptp_info;
	spinlock_t lock; /* lock for getting time */
	int reg_len;
	void __iomem *reg_addr;
	struct ptp_cipu_regs regs;
	struct ptp_cipu_stats __percpu *stats;
	time64_t epo_base_ns;
	struct workqueue_struct	*wq;
	struct mutex sync_lock;	/* lock for sync_status */
	struct work_struct sync_work;
	struct delayed_work gen_timer; /* general timer */
	struct ptp_cipu_timer_ctx timer_ctx;
	struct delayed_work mt_timer; /* maintenance check timer */
	u8 has_issue;
};

static int cipu_iowrite8_and_check(void __iomem *addr,
				   u8 value, u8 *res)
{
	iowrite8(value, addr);
	if (value != ioread8(addr))
		return -EIO;
	*res = value;
	return 0;
}

static int cipu_iowrite32_and_check(void __iomem *addr,
				    u32 value, u32 *res)
{
	iowrite32(value, addr);
	if (value != ioread32(addr))
		return -EIO;
	*res = value;
	return 0;
}

static void ptp_cipu_print_dev_events(struct ptp_cipu_ctx *ptp_ctx,
				      int event)
{
	struct device *dev = &ptp_ctx->pdev->dev;
	int type = PTP_CIPU_EVT_TYPE_DEV;

	switch (event) {
	case PTP_CIPU_EVT_H_CLK_ABN:
		PTP_CIPU_LOG_SUB(dev, KERN_ERR, type, event,
				 "Atomic Clock Error Detected\n");
		break;
	case PTP_CIPU_EVT_H_CLK_ABN_REC:
		PTP_CIPU_LOG_SUB(dev, KERN_INFO, type, event,
				 "Atomic Clock Error Recovered\n");
		break;
	case PTP_CIPU_EVT_H_DEV_MT:
		PTP_CIPU_LOG_SUB(dev, KERN_ERR, type, event,
				 "Maintenance Exception Detected\n");
		break;
	case PTP_CIPU_EVT_H_DEV_MT_REC:
		PTP_CIPU_LOG_SUB(dev, KERN_INFO, type, event,
				 "Maintenance Exception Recovered\n");
		break;
	case PTP_CIPU_EVT_H_DEV_MT_TOUT:
		PTP_CIPU_LOG_SUB(dev, KERN_INFO, type, event,
				 "Maintenance Exception Failed to Recover "
				 "within %d us\n", ptp_ctx->regs.mt_tout_us);
		break;
	case PTP_CIPU_EVT_H_DEV_BUSY:
		PTP_CIPU_LOG_SUB(dev, KERN_ERR, type, event,
				 "PHC Busy Detected\n");
		break;
	case PTP_CIPU_EVT_H_DEV_BUSY_REC:
		PTP_CIPU_LOG_SUB(dev, KERN_INFO, type, event,
				 "PHC Busy Recovered\n");
		break;
	case PTP_CIPU_EVT_H_DEV_ERR:
		PTP_CIPU_LOG_SUB(dev, KERN_ERR, type, event,
				 "PHC Error Detected\n");
		break;
	case PTP_CIPU_EVT_H_DEV_ERR_REC:
		PTP_CIPU_LOG_SUB(dev, KERN_INFO, type, event,
				 "PHC Error Recovered\n");
		break;
	default:
		break;
	}
}

static void ptp_cipu_print_ptp_events(struct ptp_cipu_ctx *ptp_ctx,
				      int event)
{
	struct device *dev = &ptp_ctx->pdev->dev;
	int type = PTP_CIPU_EVT_TYPE_PTP;

	switch (event) {
	case PTP_CIPU_EVT_P_GT_OPS:
		/* no action required */
		break;
	case PTP_CIPU_EVT_P_GT_INVAL:
		PTP_CIPU_LOG_SUB(dev, KERN_WARNING, type, event,
				 "PHC Get Time Failed Due to Invalid "
				 "Timestamp\n");
		break;
	case PTP_CIPU_EVT_P_GT_TOUT:
		PTP_CIPU_LOG_SUB(dev, KERN_WARNING, type, event,
				 "PHC Get Time Failed Due to Register Read "
				 "Timeout\n");
		break;
	case PTP_CIPU_EVT_P_GT_ETHRESH:
		/* aggregate output to timer */
		break;
	default:
		break;
	}
}

static int ptp_cipu_record_events(struct ptp_cipu_ctx *ptp_ctx,
				  int type, int event)
{
	switch (type) {
	case PTP_CIPU_EVT_TYPE_PTP:
		if (event >= PTP_CIPU_EVT_P_MAX)
			goto out;
		this_cpu_inc(ptp_ctx->stats->ptp_evts[event]);
		ptp_cipu_print_ptp_events(ptp_ctx, event);
		break;
	case PTP_CIPU_EVT_TYPE_DEV:
		if (event >= PTP_CIPU_EVT_H_MAX)
			goto out;
		this_cpu_inc(ptp_ctx->stats->dev_evts[event]);
		ptp_cipu_print_dev_events(ptp_ctx, event);
		break;
	case PTP_CIPU_EVT_TYPE_DRV:
		if (event >= PTP_CIPU_EVT_D_MAX)
			goto out;
		break;
	default:
		type = 0xff;
		goto out;
	}
	return 0;

out:
	PTP_CIPU_LOG_SUB(&ptp_ctx->pdev->dev, KERN_WARNING,
			 type, event, "Invalid Events\n");
	return -EINVAL;
}

/* process exception or recovery, must be protected by sync_lock */
static int ptp_cipu_process_sync_status(struct ptp_cipu_ctx *ptp_ctx)
{
	struct ptp_cipu_regs *regs = &ptp_ctx->regs;
	u32 last_status, status, diff_status;

	last_status = regs->sync_stat;
	regs->sync_stat = ioread32(ptp_ctx->reg_addr + PTP_CIPU_REG(sync_stat));
	status = regs->sync_stat;

	ptp_ctx->has_issue = status & PTP_CIPU_M_ST_PHC_ISSUE ? 1 : 0;

	diff_status = last_status ^ status;

	if (diff_status & PTP_CIPU_M_ST_CLK_ABN)
		ptp_cipu_record_events(ptp_ctx, PTP_CIPU_EVT_TYPE_DEV,
				       (status & PTP_CIPU_M_ST_CLK_ABN) ?
				       PTP_CIPU_EVT_H_CLK_ABN :
				       PTP_CIPU_EVT_H_CLK_ABN_REC);

	if (diff_status & PTP_CIPU_M_ST_DEV_BUSY)
		ptp_cipu_record_events(ptp_ctx, PTP_CIPU_EVT_TYPE_DEV,
				       (status & PTP_CIPU_M_ST_DEV_BUSY) ?
				       PTP_CIPU_EVT_H_DEV_BUSY :
				       PTP_CIPU_EVT_H_DEV_BUSY_REC);

	if (diff_status & PTP_CIPU_M_ST_DEV_ERR)
		ptp_cipu_record_events(ptp_ctx, PTP_CIPU_EVT_TYPE_DEV,
				       (status & PTP_CIPU_M_ST_DEV_ERR) ?
				       PTP_CIPU_EVT_H_DEV_ERR :
				       PTP_CIPU_EVT_H_DEV_ERR_REC);

	if (diff_status & PTP_CIPU_M_ST_DEV_MT) {
		ptp_cipu_record_events(ptp_ctx, PTP_CIPU_EVT_TYPE_DEV,
				       (status & PTP_CIPU_M_ST_DEV_MT) ?
				       PTP_CIPU_EVT_H_DEV_MT :
				       PTP_CIPU_EVT_H_DEV_MT_REC);
		if (status & PTP_CIPU_M_ST_DEV_MT)
			/* Maintenance exception occurs, start a timer
			 * to check whether the maintenance status can
			 * be recovered within time mt_tout_us.
			 */
			queue_delayed_work(ptp_ctx->wq, &ptp_ctx->mt_timer,
					   usecs_to_jiffies(regs->mt_tout_us));
		else if (!(status & PTP_CIPU_M_ST_DEV_MT))
			/* Maintenance exception recovered. */
			cancel_delayed_work(&ptp_ctx->mt_timer);
	}
	return status;
}

static void ptp_cipu_gen_timer(struct work_struct *work)
{
	struct ptp_cipu_ctx *ptp_ctx = container_of(to_delayed_work(work),
						    struct ptp_cipu_ctx,
						    gen_timer);
	int cpu, thresh, last, now = 0;
	struct ptp_cipu_stats *stats;

	for_each_possible_cpu(cpu) {
		stats = per_cpu_ptr(ptp_ctx->stats, cpu);
		now += stats->ptp_evts[PTP_CIPU_EVT_P_GT_ETHRESH];
	}
	last = ptp_ctx->timer_ctx.over_thresh_cnt;
	thresh = ptp_ctx->timer_ctx.thresh_us;

	ptp_ctx->timer_ctx.over_thresh_cnt = now;
	ptp_ctx->timer_ctx.thresh_us = ptp_ctx->regs.thresh_us;

	if (now > last)
		PTP_CIPU_LOG_SUB(&ptp_ctx->pdev->dev, KERN_WARNING,
				 PTP_CIPU_EVT_TYPE_PTP,
				 PTP_CIPU_EVT_P_GT_ETHRESH,
				 "Offset of PHC and System Time Exceeds "
				 "%d us %d time(s)\n",
				 thresh, now - last);
	mod_delayed_work(ptp_ctx->wq, &ptp_ctx->gen_timer,
			 PTP_CIPU_TIMER_PERIOD);
}

static void ptp_cipu_mt_timer(struct work_struct *work)
{
	struct ptp_cipu_ctx *ptp_ctx = container_of(to_delayed_work(work),
						    struct ptp_cipu_ctx,
						    mt_timer);
	u32 sync_stat;

	mutex_lock(&ptp_ctx->sync_lock);
	sync_stat = ptp_cipu_process_sync_status(ptp_ctx);
	if (sync_stat & PTP_CIPU_M_ST_DEV_MT)
		ptp_cipu_record_events(ptp_ctx, PTP_CIPU_EVT_TYPE_DEV,
				       PTP_CIPU_EVT_H_DEV_MT_TOUT);
	mutex_unlock(&ptp_ctx->sync_lock);
}

static void ptp_cipu_sync_status_work(struct work_struct *work)
{
	struct ptp_cipu_ctx *ptp_ctx =
		container_of(work, struct ptp_cipu_ctx, sync_work);

	mutex_lock(&ptp_ctx->sync_lock);
	ptp_cipu_process_sync_status(ptp_ctx);
	mutex_unlock(&ptp_ctx->sync_lock);
}

static irqreturn_t ptp_cipu_status_handler(int irq, void *data)
{
	struct ptp_cipu_ctx *ptp_ctx = (struct ptp_cipu_ctx *)data;

	queue_work(ptp_ctx->wq, &ptp_ctx->sync_work);
	return IRQ_HANDLED;
}

static irqreturn_t ptp_cipu_update_threshold(int irq, void *data)
{
	struct ptp_cipu_ctx *ptp_ctx = (struct ptp_cipu_ctx *)data;

	ptp_ctx->regs.thresh_us =
		ioread32(ptp_ctx->reg_addr + PTP_CIPU_REG(thresh_us));
	mod_delayed_work(ptp_ctx->wq, &ptp_ctx->gen_timer, 0);
	return IRQ_HANDLED;
}

static struct ptp_cipu_irq_ctx irq_ctx[PTP_CIPU_IRQ_NUM] = {
	[PTP_CIPU_IRQ_0] = { .irq_func = ptp_cipu_update_threshold },
	[PTP_CIPU_IRQ_1] = { .irq_func = ptp_cipu_status_handler }
};

static int __ptp_cipu_gettimex(struct ptp_cipu_ctx *ptp_ctx,
			       struct timespec64 *ts,
			       struct ptp_system_timestamp *sts)
{
	struct ptp_cipu_regs *regs = &ptp_ctx->regs;
	ktime_t tstamp_ns, intvl_ns, sys_ns, phc_ns;
	struct timespec64 intvl;
	unsigned long flags;

	ptp_cipu_record_events(ptp_ctx, PTP_CIPU_EVT_TYPE_PTP,
			       PTP_CIPU_EVT_P_GT_OPS);

	spin_lock_irqsave(&ptp_ctx->lock, flags);
	ptp_read_system_prets(sts);
	tstamp_ns = readq(ptp_ctx->reg_addr + PTP_CIPU_REG(tstamp_ns));
	ptp_read_system_postts(sts);
	spin_unlock_irqrestore(&ptp_ctx->lock, flags);

	if (tstamp_ns & PTP_CIPU_M_TS_ABN) {
		/* invalid timestamp */
		ptp_cipu_record_events(ptp_ctx, PTP_CIPU_EVT_TYPE_PTP,
				       PTP_CIPU_EVT_P_GT_INVAL);
		queue_work(ptp_ctx->wq, &ptp_ctx->sync_work);
		return -EIO;
	}

	/* PHC had issues before, but timestamp is currently valid,
	 * which means that there is an update of the sync_status to process.
	 */
	if (ptp_ctx->has_issue)
		queue_work(ptp_ctx->wq, &ptp_ctx->sync_work);

	intvl = timespec64_sub(sts->post_ts, sts->pre_ts);
	intvl_ns = timespec64_to_ns(&intvl);
	if (abs(intvl_ns) > regs->max_lat_ns) {
		/* register read timeout */
		ptp_cipu_record_events(ptp_ctx, PTP_CIPU_EVT_TYPE_PTP,
				       PTP_CIPU_EVT_P_GT_TOUT);
		return -EIO;
	}

	sys_ns = timespec64_to_ns(&sts->pre_ts) + intvl_ns / 2;
	phc_ns = ptp_ctx->epo_base_ns + (tstamp_ns &
		~(PTP_CIPU_M_TS_ABN | PTP_CIPU_M_TS_RESVD));

	if (abs(phc_ns - sys_ns) >
	    ptp_ctx->timer_ctx.thresh_us * NSEC_PER_USEC)
		/* time drifting exceeds the threshold, just record it */
		ptp_cipu_record_events(ptp_ctx, PTP_CIPU_EVT_TYPE_PTP,
				       PTP_CIPU_EVT_P_GT_ETHRESH);

	*ts = ns_to_timespec64(phc_ns);
	return 0;
}

static int ptp_cipu_gettimex(struct ptp_clock_info *ptp, struct timespec64 *ts,
			     struct ptp_system_timestamp *sts_user)
{
	struct ptp_cipu_ctx *ptp_ctx =
			container_of(ptp, struct ptp_cipu_ctx, ptp_info);
	struct ptp_system_timestamp sts_stack, *sts;

	sts = sts_user ? sts_user : &sts_stack;

	return __ptp_cipu_gettimex(ptp_ctx, ts, sts);
}

static int ptp_cipu_gettime(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	struct ptp_cipu_ctx *ptp_ctx =
			container_of(ptp, struct ptp_cipu_ctx, ptp_info);
	struct ptp_system_timestamp sts;

	return __ptp_cipu_gettimex(ptp_ctx, ts, &sts);
}

static int ptp_cipu_enable(struct ptp_clock_info *info,
			   struct ptp_clock_request *request, int on)
{
	return -EOPNOTSUPP;
}

static int ptp_cipu_settime(struct ptp_clock_info *p,
			    const struct timespec64 *ts)
{
	return -EOPNOTSUPP;
}

static int ptp_cipu_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	return -EOPNOTSUPP;
}

static int ptp_cipu_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	return -EOPNOTSUPP;
}

static struct ptp_clock_info ptp_cipu_clock_info = {
	.owner		= THIS_MODULE,
	.name		= "ptp_cipu",
	.adjtime	= ptp_cipu_adjtime,
	.adjfine	= ptp_cipu_adjfine,
	.gettimex64	= ptp_cipu_gettimex,
	.gettime64	= ptp_cipu_gettime,
	.settime64	= ptp_cipu_settime,
	.enable		= ptp_cipu_enable,
};

static int ptp_cipu_set_guest_features(struct ptp_cipu_ctx *ptp_ctx)
{
	struct ptp_cipu_regs *regs = &ptp_ctx->regs;

	regs->dev_feat = ioread32(ptp_ctx->reg_addr + PTP_CIPU_REG(dev_feat));

	return cipu_iowrite32_and_check(ptp_ctx->reg_addr +
					PTP_CIPU_REG(gst_feat),
					PTP_CIPU_DRV_CAP & regs->dev_feat,
					&regs->gst_feat);
}

static int ptp_cipu_set_dev_status(struct ptp_cipu_ctx *ptp_ctx,
				   int status)
{
	return cipu_iowrite8_and_check(ptp_ctx->reg_addr +
				       PTP_CIPU_REG(dev_stat), status,
				       &ptp_ctx->regs.dev_stat);
}

static int ptp_cipu_set_drv_version(struct ptp_cipu_ctx *ptp_ctx)
{
	struct ptp_cipu_regs *regs = &ptp_ctx->regs;
	int version, patchlevel, sublevel;
	u32 env_ver, drv_ver;
	int rc;

	if (sscanf(utsname()->release, "%u.%u.%u",
		   &version, &patchlevel, &sublevel) != 3)
		return -EINVAL;
	sublevel = sublevel < 0xFF ? sublevel : 0xFF;

	env_ver = (LINUX_UPSTREAM << 27) | (version << 16) |
		  (patchlevel << 8) | sublevel;

	rc = cipu_iowrite32_and_check(ptp_ctx->reg_addr +
				      PTP_CIPU_REG(env_ver),
				      env_ver, &regs->env_ver);
	if (rc)
		return rc;

	drv_ver = (DRV_TYPE << 24) | (DRV_VER_MAJOR << 16) |
		  (DRV_VER_MINOR << 8) | DRV_VER_SUBMINOR;

	return cipu_iowrite32_and_check(ptp_ctx->reg_addr +
					PTP_CIPU_REG(drv_ver), drv_ver,
					&regs->drv_ver);
}

static int ptp_cipu_init_context(struct ptp_cipu_ctx *ptp_ctx)
{
	struct ptp_cipu_regs *regs = &ptp_ctx->regs;
	void __iomem *reg_addr = ptp_ctx->reg_addr;
	int rc = -ENOMEM;

	spin_lock_init(&ptp_ctx->lock);
	mutex_init(&ptp_ctx->sync_lock);
	ptp_ctx->has_issue = 0;
	INIT_WORK(&ptp_ctx->sync_work, ptp_cipu_sync_status_work);
	INIT_DELAYED_WORK(&ptp_ctx->gen_timer, ptp_cipu_gen_timer);
	INIT_DELAYED_WORK(&ptp_ctx->mt_timer, ptp_cipu_mt_timer);

	ptp_ctx->stats = alloc_percpu(struct ptp_cipu_stats);
	if (!ptp_ctx->stats)
		return rc;

	ptp_ctx->wq = alloc_workqueue("ptp-cipu-wq", 0, 0);
	if (!ptp_ctx->wq)
		goto out_stats;

	rc = ptp_cipu_set_guest_features(ptp_ctx);
	if (rc)
		goto out_wq;

	rc = ptp_cipu_set_drv_version(ptp_ctx);
	if (rc)
		goto out_wq;

	regs->tm_prec_ns = ioread32(reg_addr + PTP_CIPU_REG(tm_prec_ns));
	regs->max_lat_ns = ioread32(reg_addr + PTP_CIPU_REG(max_lat_ns));
	regs->mt_tout_us = ioread32(reg_addr + PTP_CIPU_REG(mt_tout_us));
	regs->thresh_us = ioread32(reg_addr + PTP_CIPU_REG(thresh_us));

	if (regs->gst_feat & PTP_CIPU_M_FT_EPOCH) {
		regs->epo_base_yr = ioread32(reg_addr +
					    PTP_CIPU_REG(epo_base_yr));
		ptp_ctx->epo_base_ns = NSEC_PER_SEC *
				mktime64(regs->epo_base_yr, 1, 1, 0, 0, 0);
	}

	/* currently we don't support TAI */
	if (regs->gst_feat & PTP_CIPU_M_FT_TAI)
		regs->leap_sec = ioread32(reg_addr + PTP_CIPU_REG(leap_sec));

	ptp_ctx->timer_ctx.thresh_us = regs->thresh_us;
	queue_delayed_work(ptp_ctx->wq, &ptp_ctx->gen_timer,
			   PTP_CIPU_TIMER_PERIOD);
	return 0;

out_wq:
	destroy_workqueue(ptp_ctx->wq);
out_stats:
	free_percpu(ptp_ctx->stats);
	return rc;
}

static void ptp_cipu_clear_context(struct ptp_cipu_ctx *ptp_ctx)
{
	cancel_delayed_work_sync(&ptp_ctx->gen_timer);
	cancel_delayed_work_sync(&ptp_ctx->mt_timer);
	cancel_work_sync(&ptp_ctx->sync_work);
	destroy_workqueue(ptp_ctx->wq);
	free_percpu(ptp_ctx->stats);
}

static ssize_t register_snapshot_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct ptp_cipu_ctx *ctx = pci_get_drvdata(to_pci_dev(dev));
	struct ptp_cipu_regs *regs = &ctx->regs;

	return sysfs_emit(buf, "%s 0x%x %s 0x%x %s 0x%x %s 0x%x "
			  "%s 0x%x %s 0x%x %s 0x%x %s 0x%x %s 0x%x "
			  "%s 0x%x %s 0x%x %s 0x%x\n",
			  "device_features", regs->dev_feat,
			  "guest_features", regs->gst_feat,
			  "driver_version", regs->drv_ver,
			  "environment_version", regs->env_ver,
			  "device_status", regs->dev_stat,
			  "sync_status", regs->sync_stat,
			  "time_precision(ns)", regs->tm_prec_ns,
			  "epoch_base(years)", regs->epo_base_yr,
			  "leap_second(s)", regs->leap_sec,
			  "max_latency(ns)", regs->max_lat_ns,
			  "maintenance_timeout(us)", regs->mt_tout_us,
			  "offset_threshold(us)", regs->thresh_us);
}

static ssize_t driver_capacity_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	u32 drv_cap = PTP_CIPU_DRV_CAP;

	return sysfs_emit(buf, "%s 0x%04x\n", "driver_cap", drv_cap);
}

static ssize_t ptp_events_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ptp_cipu_ctx *ctx = pci_get_drvdata(to_pci_dev(dev));
	u64 sum[PTP_CIPU_EVT_P_MAX] = { 0 };
	struct ptp_cipu_stats *stats;
	int cpu, i;

	for_each_possible_cpu(cpu) {
		stats = per_cpu_ptr(ctx->stats, cpu);
		for (i = 0; i < PTP_CIPU_EVT_P_MAX; i++)
			sum[i] += stats->ptp_evts[i];
	}
	return sysfs_emit(buf, "%s 0x%llx %s 0x%llx "
			  "%s 0x%llx %s 0x%llx\n",
			  "total_gettimex_ops",
			  sum[PTP_CIPU_EVT_P_GT_OPS],
			  "gettm_fail_invalid_tstamp",
			  sum[PTP_CIPU_EVT_P_GT_INVAL],
			  "gettm_fail_timeout",
			  sum[PTP_CIPU_EVT_P_GT_TOUT],
			  "timeoff_excd_thresh",
			  sum[PTP_CIPU_EVT_P_GT_ETHRESH]);
}

static ssize_t device_events_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct ptp_cipu_ctx *ctx = pci_get_drvdata(to_pci_dev(dev));
	u64 sum[PTP_CIPU_EVT_H_MAX] = { 0 };
	struct ptp_cipu_stats *stats;
	int cpu, i;

	for_each_possible_cpu(cpu) {
		stats = per_cpu_ptr(ctx->stats, cpu);
		for (i = 0; i < PTP_CIPU_EVT_H_MAX; i++)
			sum[i] += stats->dev_evts[i];
	}
	return sysfs_emit(buf, "%s 0x%llx %s 0x%llx %s 0x%llx "
			  "%s 0x%llx %s 0x%llx %s 0x%llx %s 0x%llx "
			  "%s 0x%llx %s 0x%llx\n",
			  "clock_abnormal",
			  sum[PTP_CIPU_EVT_H_CLK_ABN],
			  "clock_abnormal_recov",
			  sum[PTP_CIPU_EVT_H_CLK_ABN_REC],
			  "device_maintenance",
			  sum[PTP_CIPU_EVT_H_DEV_MT],
			  "device_maintenance_recov",
			  sum[PTP_CIPU_EVT_H_DEV_MT_REC],
			  "device_maintenance_timeout",
			  sum[PTP_CIPU_EVT_H_DEV_MT_TOUT],
			  "device_busy",
			  sum[PTP_CIPU_EVT_H_DEV_BUSY],
			  "device_busy_recov",
			  sum[PTP_CIPU_EVT_H_DEV_BUSY_REC],
			  "device_error",
			  sum[PTP_CIPU_EVT_H_DEV_ERR],
			  "device_error_recov",
			  sum[PTP_CIPU_EVT_H_DEV_ERR_REC]);
}

static DEVICE_ATTR_RO(register_snapshot);
static DEVICE_ATTR_RO(driver_capacity);
static DEVICE_ATTR_RO(ptp_events);
static DEVICE_ATTR_RO(device_events);

static struct attribute *ptp_cipu_attrs[] = {
	&dev_attr_register_snapshot.attr,
	&dev_attr_driver_capacity.attr,
	&dev_attr_ptp_events.attr,
	&dev_attr_device_events.attr,
	NULL,
};

static const struct attribute_group ptp_cipu_attr_group = {
	.attrs = ptp_cipu_attrs,
	.name = "cipu_stats",
};

static int ptp_cipu_init_sysfs(struct pci_dev *pdev)
{
	return sysfs_create_group(&pdev->dev.kobj, &ptp_cipu_attr_group);
}

static void ptp_cipu_remove_sysfs(struct pci_dev *pdev)
{
	sysfs_remove_group(&pdev->dev.kobj, &ptp_cipu_attr_group);
}

static int ptp_cipu_init_irq(struct ptp_cipu_ctx *ptp_ctx)
{
	struct pci_dev *pdev = ptp_ctx->pdev;
	int i, rc;

	rc = pci_alloc_irq_vectors(pdev, PTP_CIPU_IRQ_NUM,
				   PTP_CIPU_IRQ_NUM, PCI_IRQ_MSIX);
	if (rc < 0)
		goto out;

	for (i = 0; i < PTP_CIPU_IRQ_NUM; i++) {
		rc = pci_request_irq(pdev, i, irq_ctx[i].irq_func, NULL,
				     ptp_ctx, "ptp-cipu");
		if (rc)
			goto out_vec;
	}
	return 0;

out_vec:
	for (i = i - 1; i >= 0; i--)
		pci_free_irq(pdev, i, ptp_ctx);
	pci_free_irq_vectors(pdev);
out:
	return rc;
}

static void ptp_cipu_clear_irq(struct ptp_cipu_ctx *ptp_ctx)
{
	struct pci_dev *pdev = ptp_ctx->pdev;
	int i;

	for (i = 0; i < PTP_CIPU_IRQ_NUM; i++)
		pci_free_irq(pdev, i, ptp_ctx);
	pci_free_irq_vectors(pdev);
}

static int ptp_cipu_probe(struct pci_dev *pdev,
			  const struct pci_device_id *id)
{
	struct ptp_cipu_ctx *ptp_ctx;
	int rc;

	ptp_ctx = kzalloc(sizeof(*ptp_ctx), GFP_KERNEL);
	if (!ptp_ctx) {
		rc = -ENOMEM;
		PTP_CIPU_LOG(&pdev->dev, KERN_ERR, PTP_CIPU_EVT_D_PROBE_FAIL,
			     "Context Allocated Error: %d\n", rc);
		return rc;
	}
	ptp_ctx->pdev = pdev;
	pci_set_drvdata(pdev, ptp_ctx);

	rc = pci_enable_device(pdev);
	if (rc) {
		PTP_CIPU_LOG(&pdev->dev, KERN_ERR, PTP_CIPU_EVT_D_PROBE_FAIL,
			     "PCI Device Enabled Fail: %d\n", rc);
		goto out_mem;
	}

	ptp_ctx->reg_len = sizeof(struct ptp_cipu_regs);
	ptp_ctx->reg_addr = pci_iomap(pdev, PTP_CIPU_BAR_0, ptp_ctx->reg_len);
	if (!ptp_ctx->reg_addr) {
		rc = -ENOMEM;
		PTP_CIPU_LOG(&pdev->dev, KERN_ERR, PTP_CIPU_EVT_D_PROBE_FAIL,
			     "PCI IO Map Error: %d\n", rc);
		goto out_enable;
	}

	rc = ptp_cipu_set_dev_status(ptp_ctx, PTP_CIPU_DEV_RESET);
	if (rc) {
		PTP_CIPU_LOG(&pdev->dev, KERN_ERR, PTP_CIPU_EVT_D_PROBE_FAIL,
			     "Initialize Device Status to %d Error: %d\n",
			     PTP_CIPU_DEV_RESET, rc);
		goto out_map;
	}

	rc = ptp_cipu_init_context(ptp_ctx);
	if (rc) {
		PTP_CIPU_LOG(&pdev->dev, KERN_ERR, PTP_CIPU_EVT_D_PROBE_FAIL,
			     "Initialize Context Error: %d\n", rc);
		goto out_map;
	}

	rc = ptp_cipu_set_dev_status(ptp_ctx, PTP_CIPU_DEV_INIT);
	if (rc) {
		PTP_CIPU_LOG(&pdev->dev, KERN_ERR, PTP_CIPU_EVT_D_PROBE_FAIL,
			     "Initialize Device Status to %d Error: %d\n",
			     PTP_CIPU_DEV_INIT, rc);
		goto out_context;
	}

	rc = ptp_cipu_init_irq(ptp_ctx);
	if (rc) {
		PTP_CIPU_LOG(&pdev->dev, KERN_ERR, PTP_CIPU_EVT_D_PROBE_FAIL,
			     "Initialize IRQ Error: %d\n", rc);
		goto out_reset;
	}

	rc = ptp_cipu_init_sysfs(pdev);
	if (rc) {
		PTP_CIPU_LOG(&pdev->dev, KERN_ERR, PTP_CIPU_EVT_D_PROBE_FAIL,
			     "Initialize Sysfs Error: %d\n", rc);
		goto out_irq;
	}

	ptp_ctx->ptp_info = ptp_cipu_clock_info;
	ptp_ctx->ptp_clock = ptp_clock_register(&ptp_ctx->ptp_info,
						&pdev->dev);
	if (IS_ERR(ptp_ctx->ptp_clock)) {
		rc = PTR_ERR(ptp_ctx->ptp_clock);
		PTP_CIPU_LOG(&pdev->dev, KERN_ERR, PTP_CIPU_EVT_D_PROBE_FAIL,
			     "PTP Clock Register Error: %d\n", rc);
		goto out_sysfs;
	}

	/* all set, enable irqs */
	rc = ptp_cipu_set_dev_status(ptp_ctx, PTP_CIPU_DEV_READY);
	if (rc) {
		PTP_CIPU_LOG(&pdev->dev, KERN_ERR, PTP_CIPU_EVT_D_PROBE_FAIL,
			     "Initialize Device Status to %d Error: %d\n",
			     PTP_CIPU_DEV_READY, rc);
		goto out_clock;
	}

	PTP_CIPU_LOG(&pdev->dev, KERN_INFO, PTP_CIPU_EVT_D_GENERAL,
		     "Alibaba CIPU PHC Driver Loaded, Version 0x%x, 0x%x\n",
		     ptp_ctx->regs.drv_ver, ptp_ctx->regs.env_ver);
	return rc;

out_clock:
	ptp_clock_unregister(ptp_ctx->ptp_clock);
out_sysfs:
	ptp_cipu_remove_sysfs(pdev);
out_irq:
	ptp_cipu_clear_irq(ptp_ctx);
out_reset:
	ptp_cipu_set_dev_status(ptp_ctx, PTP_CIPU_DEV_RESET);
out_context:
	ptp_cipu_clear_context(ptp_ctx);
out_map:
	pci_iounmap(pdev, ptp_ctx->reg_addr);
out_enable:
	pci_disable_device(pdev);
out_mem:
	kfree(ptp_ctx);
	ptp_ctx = NULL;
	return rc;
}

static void ptp_cipu_remove(struct pci_dev *pdev)
{
	struct ptp_cipu_ctx *ptp_ctx = pci_get_drvdata(pdev);

	ptp_clock_unregister(ptp_ctx->ptp_clock);
	ptp_cipu_remove_sysfs(pdev);
	ptp_cipu_set_dev_status(ptp_ctx, PTP_CIPU_DEV_RESET); /* disable irqs */
	ptp_cipu_clear_irq(ptp_ctx);
	ptp_cipu_clear_context(ptp_ctx); /* wait for timer/worker to finish */
	pci_iounmap(pdev, ptp_ctx->reg_addr);
	pci_disable_device(pdev);
	kfree(ptp_ctx);
	PTP_CIPU_LOG(&pdev->dev, KERN_INFO, PTP_CIPU_EVT_D_GENERAL,
		     "Alibaba CIPU PHC Driver Unloaded\n");
}

static const struct pci_device_id ptp_cipu_pci_tbl[] = {
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_ALIBABA, 0x500C,
			 PCI_VENDOR_ID_ALIBABA, 0x1123) },
	{ }
};
MODULE_DEVICE_TABLE(pci, ptp_cipu_pci_tbl);

static struct pci_driver ptp_cipu_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= ptp_cipu_pci_tbl,
	.probe		= ptp_cipu_probe,
	.remove		= ptp_cipu_remove,
};

static int __init ptp_cipu_init(void)
{
	return pci_register_driver(&ptp_cipu_driver);
}

static void __exit ptp_cipu_exit(void)
{
	pci_unregister_driver(&ptp_cipu_driver);
}

module_init(ptp_cipu_init);
module_exit(ptp_cipu_exit);

MODULE_DESCRIPTION("PTP clock for Alibaba CIPU");
MODULE_AUTHOR("Wen Gu <guwen@linux.alibaba.com>");
MODULE_LICENSE("GPL");
