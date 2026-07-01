/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020 Xing Loong <xing.xl.loong@gmail.com>
 */
#ifndef MBEDTEE_DRV_H
#define MBEDTEE_DRV_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/atomic.h>
#include <linux/of.h>
#include <linux/ioport.h>
#include <linux/tee_drv.h>
#include <linux/tee_core.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/msi.h>
#include <linux/workqueue.h>
#include <linux/cpuhotplug.h>
#include <linux/cpumask.h>
#include <linux/xarray.h>
#include "mbedtee_msg.h"
#include "shm_pool.h"

/*
 * GlobalPlatform TEE Client API return codes/origins
 * used by this driver. Only the codes actually
 * referenced in the driver are defined here.
 */
#define TEEC_SUCCESS				0x00000000
#define TEEC_ERROR_OUT_OF_MEMORY	0xFFFF000C
#define TEEC_ERROR_COMMUNICATION	0xFFFF000E

#define TEEC_ORIGIN_COMMS			0x00000002

#define MBEDTEE_VERSION_MAJOR		1
#define MBEDTEE_VALID_VERSION(x)	(((x) >> 16) == MBEDTEE_VERSION_MAJOR)

/* Maximum payload size for inline (async) RPC data. */
#define MBEDTEE_ASYNC_RPC_DATA_MAX	256

struct rpc_transport_ops;
struct mbedtee_rpc_call;
struct mbedtee_supp_req;
struct mbedtee_device;

struct rpc_work {
	struct work_struct work;
	struct mbedtee_device *mbedtee;
	u64 waiter_id;
	size_t size;
	bool complete_only;
	/* Points to ctx->rpc_data (async) or into t2r_shm (sync) */
	void *data;
	void (*func)(struct mbedtee_device *mbedtee, void *data, size_t size);
};

struct mbedtee_t2r_ctx {
	spinlock_t ring_lock; /* protects T2R ring producer/consumer state */
	void *t2r_shm;
	phys_addr_t t2r_shm_phys;
	resource_size_t t2r_shm_sz;
	struct rpc_ringbuf *t2r_ring;
	u32 t2r_ring_sz;
	u32 t2r_ring_rd;

	void (*rpc_handlers[MBEDTEE_RPC_MAX])(struct mbedtee_device *mbedtee,
					      void *data, size_t size);

	/*
	 * Inline buffer for async RPC payloads (waiter_id == 0).
	 * Sync RPC payloads (waiter_id != 0) live in shared memory (t2r_shm).
	 */
	u8 rpc_data[MBEDTEE_ASYNC_RPC_DATA_MAX];

	/*
	 * CPU-hotplug tracking for T2R interrupt delivery.
	 * callee_cpus: shadow of cpu_online_mask maintained by the core
	 *   callee layer (rpc_callee.c) for both ARM and RISC-V; used by
	 *   ARM to feed irq_set_affinity() and by RISC-V as the gate for
	 *   MSI migration decisions.
	 * callee_virq: single MSI virq currently pinned to one CPU
	 *   (RISC-V IMSIC only; 0 for ARM).
	 * hp_node: per-instance node for cpuhp_setup_state_multi().
	 */
	struct cpumask callee_cpus;
	int callee_virq;
	struct hlist_node hp_node;
	bool cpuhp_added;
	bool complete_work_pending;
	bool pending_async;
	u32 pending_size;
	struct workqueue_struct *rpc_wq;
	struct rpc_work complete_work;
	void (*pending_func)(struct mbedtee_device *mbedtee,
			     void *data, size_t size);
};

struct mbedtee_r2t_ctx {
	spinlock_t lock; /* protects R2T ring write-side state */
	struct rpc_ringbuf *ring;
	u32 ring_sz;
	u32 ring_wr;
};

struct mbedtee_rpc_transport_ctx {
	int rpc_notify_virq;
	struct msi_msg rpc_msi_msg;
};

struct mbedtee_supp {
	struct mutex mutex; /* serializes supp request queue and active requests */
	struct tee_context *ctx;
	bool shutting_down;
	struct list_head reqs;
	struct list_head active_reqs;
	struct completion reqs_c;
};

struct mbedtee_device {
	u32 version;
	bool yield;
	struct device *dev;
	struct tee_device *teedev;
	struct tee_shm_pool *pool;
	struct mbedtee_supp supp;
	struct xarray rpc_calls;
	atomic_long_t rpc_call_seq;
	const struct rpc_transport_ops *rpc_ops;
	struct mbedtee_t2r_ctx t2r;
	struct mbedtee_r2t_ctx r2t;
	struct mbedtee_rpc_transport_ctx transport;
};

struct mbedtee_rpc_call {
	struct completion c;
	phys_addr_t rpc_phys;
	u32 state;
	/*
	 * Wire-format rpc_cmd with inline data[] payload.
	 * MUST be the last field: rpc.data[] is a flexible array.
	 */
	struct rpc_cmd rpc;
};

struct mbedtee_session {
	struct list_head list_node;
	u32 session_id;
};

struct mbedtee_context_data {
	bool is_supp_ctx;
	struct mutex mutex; /* serializes session list updates */
	struct list_head sess_list;
};

static inline phys_addr_t mbedtee_virt_to_phys(void *va)
{
	if (is_vmalloc_or_module_addr(va))
		return page_to_phys(vmalloc_to_page(va)) +
		       offset_in_page(va);

	return virt_to_phys(va);
}

int mbedtee_get_resource(struct device_node *node,
			 const char *name, struct resource *res);

/*
 * RPC transport operations -- implemented per-architecture.
 *
 * Caller (rpc_caller_{arm,riscv}.c) platform differences:
 *   fastcall:       ARM/ARM64 - direct SMC (arm_smccc_smc), synchronous,
 *                   never returns -ENOSPC.
 *                   RISC-V - writes rpc_cmd phys addr to the r2t ring
 *                   buffer, then waits for TEE completion via the t2r
 *                   ring.  Despite the "fastcall" name this is a
 *                   synchronous ring-buffer round-trip; it may return
 *                   -ENOSPC when the r2t ring is full, and the caller
 *                   must retry (see rpc_routine() for the
 *                   MBEDTEE_RPC_COMPLETE_TEE retry loop).
 *   yieldcall:      ARM/ARM64 - one SMC posting rpc_cmd phys addr, waits
 *                   for completion.
 *                   RISC-V - writes rpc_cmd phys addr to r2t ring, waits
 *                   for completion.
 * Callee (rpc_callee_{arm,riscv}.c) platform differences:
 *   notify:         ARM/ARM64 - GIC SPI; the TEE broadcasts via GIC
 *                   hardware (GICv2 ITARGETS=0xFF, GICv3 IROUTER.IRM=1).
 *                   Ring metadata unchanged on CPU affinity events.
 *                   RISC-V - IMSIC MSI; the driver writes callee_hartid
 *                   and callee_imsic_id into the ring so the TEE knows
 *                   where to send the MSI. The affinity handler updates
 *                   both ring fields when the callee CPU goes offline.
 *
 * init:             Set up the interrupt delivery mechanism (MSI on RISC-V,
 *                   GIC/OF-IRQ on ARM/ARM64) and, on RISC-V, write
 *                   ring->callee_hartid and ring->callee_imsic_id. @handler
 *                   is the hard-IRQ handler that drains the ring buffer and
 *                   dispatches RPC commands.
 * uninit:           Tear down and free all interrupt resources.
 * update_affinity:  Migrate T2R interrupt delivery to @new_cpu. Called by the
 *                   common cpuhp handler when the current callee CPU goes
 *                   offline. On RISC-V IMSIC, must update ring->callee_hartid
 *                   and ring->callee_imsic_id. ARM uses GIC hardware routing
 *                   and requires no ring update.
 *
 * The getter is implemented per-architecture in rpc_callee_{arm,riscv}.c;
 * only one is linked per build.
 */
struct rpc_transport_ops {
	int (*init)(struct mbedtee_device *mbedtee, struct rpc_ringbuf *ring,
		    irq_handler_t handler);
	void (*uninit)(struct mbedtee_device *mbedtee);
	int (*update_affinity)(struct mbedtee_device *mbedtee,
			       unsigned int new_cpu);
};

const struct rpc_transport_ops *mbedtee_get_rpc_transport_ops(void);

/* rpc-callee.c */
int mbedtee_rpc_init(struct mbedtee_device *mbedtee);
void mbedtee_rpc_uninit(struct mbedtee_device *mbedtee);
irqreturn_t mbedtee_rpc_irq_handler(int irq, void *dev_id);

/* rpc_caller.c */
int mbedtee_rpc_call_alloc(struct mbedtee_device *mbedtee,
			   size_t payload_size, struct mbedtee_rpc_call **call);
void mbedtee_rpc_call_free(struct mbedtee_device *mbedtee,
			   struct mbedtee_rpc_call *call);
void mbedtee_rpc_complete_call(struct mbedtee_device *mbedtee,
			       u64 waiter_id);
long mbedtee_rpc_wait_for_completion(struct mbedtee_device *mbedtee,
				     struct mbedtee_rpc_call *call, bool killable);
int mbedtee_open_session(struct tee_context *ctx,
			 struct tee_ioctl_open_session_arg *arg,
			 struct tee_param *param);
int mbedtee_close_session(struct tee_context *ctx, u32 session);
int mbedtee_invoke_func(struct tee_context *ctx,
			struct tee_ioctl_invoke_arg *arg,
			struct tee_param *param);
int mbedtee_cancel_req(struct tee_context *ctx, u32 cancel_id,
		       u32 session);
int mbedtee_shm_register(struct tee_context *ctx, struct tee_shm *shm,
			 struct page **pages, size_t num_pages, unsigned long start);
int mbedtee_shm_unregister(struct tee_context *ctx, struct tee_shm *shm);

/* rpc-caller-{arm,riscv}.c */
long mbedtee_rpc_yieldcall(struct mbedtee_device *mbedtee,
			   unsigned long fn, struct mbedtee_rpc_call *call,
			   bool interruptible);
long mbedtee_rpc_fastcall(struct mbedtee_device *mbedtee,
			  unsigned long fn, unsigned long a0,
			  unsigned long a1, unsigned long a2);
int mbedtee_r2t_init(struct mbedtee_device *mbedtee);
void mbedtee_r2t_uninit(struct mbedtee_device *mbedtee);

/* supp.c */
void mbedtee_supp_init(struct mbedtee_supp *supp);
void mbedtee_supp_uninit(struct mbedtee_supp *supp);
void mbedtee_supp_release(struct mbedtee_supp *supp, struct tee_context *ctx);
void mbedtee_supp_abort_all(struct mbedtee_supp *supp);
void mbedtee_supp_handler(struct mbedtee_device *mbedtee,
			  u32 func, void *data, size_t size);
int mbedtee_supp_recv(struct tee_context *ctx, u32 *func,
		      u32 *num_params, struct tee_param *param);
int mbedtee_supp_send(struct tee_context *ctx, u32 ret,
		      u32 num_params, struct tee_param *param);

#endif /* MBEDTEE_DRV_H */
