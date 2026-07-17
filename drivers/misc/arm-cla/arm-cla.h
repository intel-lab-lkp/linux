/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Arm CLA driver - internal definitions
 *
 * Copyright 2026 Arm Limited.
 */
#ifndef _ARM_CLA_H_
#define _ARM_CLA_H_

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kref.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mutex.h>
#include <linux/rhashtable-types.h>
#include <linux/rwsem.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "arm-cla-regs.h"

/* Number of accelerators per CLA */
#define CLA_NUM_ACC		8
#define CLA_NUM_DATA_REGS	8
#define CLA_SRSTATE_LEN		8

/* Quantum of CLA assignment */
#define CLA_SLICE_MS		100

struct cla_domain;

struct cla_call_on_cpu {
	int ret;
	struct {
		struct cla_ctx *prev_ctx;
		struct cla_ctx *next_ctx;
		unsigned int ctx_id;
	} sched;
	struct kthread_work switch_ctx;
};

/**
 * struct cla_dev - CLA device
 *
 * Immutable state:
 * @cpu:		The CPU this CLA is attached to.
 * @regs:		Registers accessed by the kernel.
 * @dev:		The platform device.
 * @pfn:		Page of registers assigned to user.
 * @accelerators:	Available accelerators.
 * @pg_offset:		Mmap offset of this device.
 * @iassizes:		Combined regstate of all accels.
 * @domain:		The domain this CLA belongs to.
 *
 * Mutable, only accessed under @lock:
 * @lock:		Protects the following members.
 * @broken:		Hardware failure.
 * @worker:		CPU-bound worker to communicate with CLA.
 * @worker_sem:		Serialize running @call against @worker destruction.
 * @call:		Scheduling work.
 */
struct cla_dev {
	unsigned int cpu;
	void __iomem *regs;
	struct device *dev;
	unsigned long pfn;
	u8 accelerators;
	unsigned long pg_offset;
	unsigned long iassizes;
	struct cla_domain *domain;

	struct mutex lock;
	bool broken;
	struct kthread_worker *worker;
	struct rw_semaphore worker_sem;
	struct cla_call_on_cpu call;
};

/**
 * struct cla_domain - Collection of cla_dev
 *
 * The whole domain is assigned to a single cla_ctx at a time.
 *
 * Immutable state:
 * @id:			Domain identifier, from FW or generated.
 * @pg_offset:		Mmap offset of the first device.
 * @nr_devs:		Number of devices in the domain.
 * @devs:		Devices.
 *
 * Mutable, only accessed under @lock:
 * @lock:		Protects the following members.
 * @ctxs:		All live contexts, keyed on mm_struct and file ptr.
 * @queued_ctxs:	Queue of contexts waiting for assignment.
 * @dying_ctxs:		Contexts waiting for reclaim.
 * @worker:		Kthread worker to coordinate reassignment.
 * @reassign:		Delayed work that switches contexts with time slicing.
 * @reclaim:		Work to release and free contexts (after reassignment).
 *
 * Mutable, some reads outside the lock:
 * @broken:		Hardware failure in any device in the domain.
 * @assigned_ctx:	Context to which domain is currently assigned.
 */
struct cla_domain {
	unsigned int id;
	unsigned long pg_offset;
	unsigned int nr_devs;
	struct cla_dev **devs;

	struct mutex lock;
	bool broken;
	struct rhashtable ctxs;
	struct list_head queued_ctxs;
	struct list_head dying_ctxs;
	struct cla_ctx *assigned_ctx;
	struct kthread_worker *worker;
	struct kthread_delayed_work reassign;
	struct kthread_delayed_work reclaim;
};

/**
 * struct cla_regs - Saved CLA register state
 *
 * @data:		DATA registers.
 * @lresp:		LRESP register.
 * @accel_valid:	Accelerator state has been saved once.
 * @srstate:		Save/restore state for each accelerator.
 * @regstate:		Internal accelerator state.
 */
struct cla_regs {
	u64 data[CLA_NUM_DATA_REGS];
	u64 lresp;
	bool accel_valid;
	u64 srstate[CLA_NUM_ACC][CLA_SRSTATE_LEN];
	u64 regstate[];
};

struct cla_ctx_key {
	struct mm_struct *mm;
	struct file *file;
};

/**
 * struct cla_ctx - Domain context
 *
 * Immutable state:
 * @domain:		The domain of this context.
 * @key:		Key in cla_domain::ctxs hashtable.
 * @node:		Node in cla_domain::ctxs hashtable.
 *
 * Mutable, protected by domain::lock:
 * @refcnt:		Current users of this context.
 * @queue_node:		Node in cla_domain::queued_ctxs or
 *			cla_domain::dying_ctxs.
 * @waitq:		Faulting threads sleep until assignment.
 *
 * Mutable, protected by domain::lock, some reads outside the lock:
 * @mapped:		Number of VMAs mapping the context.
 *
 * Mutable, written only by domain::reassign and dev::switch_ctx:
 * @regs:		State for each device in domain.
 * @asid:		Pinned ASID of live context.
 */
struct cla_ctx {
	struct kref refcnt;
	struct rhash_head node;

	struct cla_domain *domain;
	struct cla_ctx_key key;

	refcount_t mapped;
	struct list_head queue_node;
	struct cla_regs **regs;
	wait_queue_head_t waitq;
	unsigned int asid;
};

extern struct xarray cla_domains;
extern unsigned int cla_nr_domains;
extern struct cla_dev **cla_lut_cpu;
extern struct cla_dev **cla_lut_pg;
extern unsigned int cla_nr_devs;

#define cla_for_each_accid(dev, accid) \
	for ((accid) = 0; (accid) < CLA_NUM_ACC; (accid)++) \
		for_each_if((dev)->accelerators & BIT(accid))

#define cla_dbg(dev, fmt, ...) \
	dev_dbg((dev)->dev, "[%u] " fmt, (dev)->cpu, ##__VA_ARGS__)
#define cla_info(dev, fmt, ...) \
	dev_info((dev)->dev, "[%u] " fmt, (dev)->cpu, ##__VA_ARGS__)
#define cla_err(dev, fmt, ...) \
	dev_err((dev)->dev, "[%u] " fmt, (dev)->cpu, ##__VA_ARGS__)

#define CLA_REG_SIZE	SZ_64K
#define CLA_FRAME_SIZE	(4 * CLA_REG_SIZE)

/* Return the registers corresponding to this privilege level */
#define cla_get_regs(base, pl) \
	((typeof(base))((uintptr_t)(base) + (pl) * CLA_REG_SIZE))

static inline u64 cla_reg_read(struct cla_dev *dev, off_t reg)
{
	return readq_relaxed(dev->regs + reg);
}

static inline void cla_reg_write(struct cla_dev *dev, off_t reg, u64 val)
{
	return writeq_relaxed(val, dev->regs + reg);
}

/*
 * If we're at EL2, use PL2. If we're a guest or nVHE host, use PL1.
 */
#define cla_kernel_pl (is_kernel_in_hyp_mode() ? 2 : 1)

struct cla_domain *cla_dev_domain_get(struct cla_dev *dev);
int cla_domains_finalise(void);
void cla_domains_free(void);
int cla_domain_sched_init(struct cla_domain *domain);
void cla_domain_sched_exit(struct cla_domain *domain);
void cla_domain_set_broken(struct cla_domain *domain);
struct cla_ctx *cla_domain_lookup_ctx(struct cla_domain *domain,
				      struct mm_struct *mm, struct file *file);
int cla_domain_insert_ctx(struct cla_domain *domain, struct cla_ctx *ctx);
void cla_domain_remove_ctx(struct cla_domain *domain, struct cla_ctx *ctx);
void cla_domain_schedule_reassignment(struct cla_domain *domain,
				      unsigned long ms);
void cla_domain_schedule_reclaim(struct cla_domain *domain);
struct cla_ctx *cla_ctx_map(struct cla_domain *domain, struct mm_struct *mm,
			    struct file *file);
void cla_ctx_unmap(struct cla_domain *domain, struct mm_struct *mm,
		   struct file *file);
void cla_ctx_free(struct kref *ref);

static inline void cla_ctx_get(struct cla_ctx *ctx)
{
	kref_get(&ctx->refcnt);
}

static inline void cla_ctx_put(struct cla_ctx *ctx)
{
	kref_put(&ctx->refcnt, cla_ctx_free);
}

static inline bool cla_ctx_is_dying(struct cla_ctx *ctx)
{
	return refcount_read(&ctx->mapped) == 0;
}

void cla_dev_switch_ctx(struct kthread_work *work);

int cla_op_wait_lresp(struct cla_dev *dev, u64 *lresp);
int cla_op_reset(struct cla_dev *dev, unsigned int accid);
int cla_op_reset_all(struct cla_dev *dev);
int cla_op_regread(struct cla_dev *dev, unsigned int accid, unsigned int regidx,
		   size_t nregs, u64 *regs);
int cla_op_regwrite(struct cla_dev *dev, unsigned int accid,
		    unsigned int regidx, size_t nregs, u64 *regs);
int cla_op_setctx(struct cla_dev *dev, unsigned int regidx, size_t nregs,
		  u64 *regs);
int cla_op_getctx(struct cla_dev *dev, unsigned int regidx, size_t nregs,
		  u64 *regs);
int cla_op_entersr(struct cla_dev *dev, unsigned int accid, u64 *srstate);
int cla_op_exitsr(struct cla_dev *dev, unsigned int accid, u64 *srstate);

int cla_regs_switch_out(struct cla_dev *dev, struct cla_regs *regs,
			bool save_regs);
int cla_regs_switch_in(struct cla_dev *dev, struct cla_regs *regs);
struct cla_regs **cla_regs_alloc_domain(struct cla_domain *domain);
void cla_regs_free_domain(struct cla_domain *domain, struct cla_regs **regs);

int cla_mtc_setup(struct cla_dev *dev);
int cla_mtc_clear(struct cla_dev *dev);
int cla_mtc_install(struct cla_dev *dev, pgd_t *pgd, unsigned long asid);
int cla_mtc_uninstall(struct cla_dev *dev);

#endif /* _ARM_CLA_H_ */
