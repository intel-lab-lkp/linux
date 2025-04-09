// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2021 Silex Insight sa
 * Copyright (c) 2018-2021 Beerten Engineering scs
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 */

#ifndef __AMDPK_DRV_H__
#define __AMDPK_DRV_H__

#include <linux/types.h>
#include <linux/kthread.h>
#include <linux/io.h>
#include <drm/drm_drv.h>
#include <uapi/drm/amdpk.h>

/* Magic number in the AMD PKI device, required to validate hardware access. */
#define AMDPK_MAGIC 0x5113C50C

/* Contains magic number 0x5113C5OC.
 * Used to validate access to the hardware registers.
 */
#define REG_MAGIC (0x00)

/* Contains version of the hardware interface as semver.
 * The semantic version : major 8 bits, minor 8 bits in little endian order.
 */
#define REG_SEMVER (0x08)

/* The number of request queues available in the hardware. */
#define REG_CFG_REQ_QUEUES_CNT 0x10

/* The maximum number of pending requests from all request queues combined. */
#define REG_CFG_MAX_PENDING_REQ 0x18

/* The maximum number of pending requests in a single request queue. */
#define REG_CFG_MAX_REQ_QUEUE_ENTRIES 0x0020

/* The first 16 bits give the amount of PK core instances with 64 multipliers.
 * The next 16 bits give the amount of PK core instances with 256 multipliers.
 */
#define REG_CFG_PK_INST 0x28

/* Writing 0x1 puts all pkcore accelerators and scheduler in reset.
 * Writing 0x0 makes all pkcore accelerators and scheduler leave reset
 * and become operational.
 */
#define REG_PK_GLOBAL_STATE 0x38

/* The semantic version : major 8 bits, minor 8 bits,
 * scm id 16 bits in little endian order.
 */
#define REG_HW_VERSION (0x40)

/* Bitmask of which CQ interrupts are raised. */
#define REG_PK_IRQ_STATUS 0x88

/* Bitmask of which CQ may trigger interrupts. */
#define REG_IRQ_ENABLE 0x90

/* Bitmask of CQ interrupts to reset. */
#define REG_PK_IRQ_RESET 0xA0

/* Bus address of the page p for the given request queue.
 * The address must be aligned on the page size.
 */
#define REG_RQ_CFG_PAGE(qid, pageidx) (0x00100 + (qid) * 0x80 + (pageidx) * 0x8)

/* Size in bytes of the pages represented as a power of 2.
 *
 * Allowed values :
 * ================ ==============
 *  register value   size in bytes
 * ================ ==============
 *     7               128
 *     8               256
 *     9               512
 *    10              1024
 *    11              2048
 *    12              4096
 *    13              8192
 *    14             16384
 *    15             32768
 *    16             65536
 * ================ ==============
 */
#define REG_RQ_CFG_PAGE_SIZE(qid) (0x00120 + (qid) * 0x80)

/* Index of the associated completion queue. */
#define REG_RQ_CFG_CQID(qid) (0x00128 + (qid) * 0x80)

/* Bit field of pages where descriptor can write to.
 * When a bit is 1, a descriptor can write to the corresponding page.
 */
#define REG_RQ_CFG_PAGES_WREN(qid) (0x00138 + (qid) * 0x80)

/* Maximum number of entries which can be written into this request queue. */
#define REG_RQ_CFG_DEPTH(qid) (0x00140 + (qid) * 0x80)

/* Bus address of the ring base of completion queue n.
 * The address must be aligned on 64 bits.
 */
#define REG_CQ_CFG_ADDR(qid) (0x1100 + (qid) * 0x80)

/* CQ notification trigger position. */
#define REG_CTL_CQ_NTFY(qid) (0x2028 + (qid) * 0x1000)

/* Size in bytes of the completion ring represented as a power of 2.
 *
 * Allowed sizes :
 * ================ ============== ==============
 *  register value   size in bytes  max entries
 * ================ ============== ==============
 *      7             128             16
 *      8             256             32
 *      9             512             64
 *     10            1024            128
 *     11            2048            256
 *     12            4096            512
 *     13            8192           1024
 *     14           16384           2048
 * ================ ============== ==============
 */
#define REG_CQ_CFG_SIZE(qid) (0x1108 + (qid) * 0x80)

/* Interrupt number for this completion queue. */
#define REG_CQ_CFG_IRQ_NR(qid) (0x1110 + (qid) * 0x80)

/* Control registers base address for the given request completion queue pair. */
#define REG_CTL_BASE(qid) (0x2000 + (qid) * 0x1000)

/* Count of how many requests are queued at a given time for this RQCQ.
 * When this count reaches 0, the resources of the request and
 * completion queues can be deleted.
 */
#define REG_CTL_PENDING_REQS  0x18

/* Busy cycle count register address. */
#define REG_PK_BUSY_CYCLES 0x2108
/* Busy cycle count  register address.*/
#define REG_PK_IDLE_CYCLES 0x2110

/* Hardware interface versions. */
#define AMDPK_SEMVER_MAJOR(v) (((v) >> 24) & 0xff)
#define AMDPK_SEMVER_MINOR(v) (((v) >> 16) & 0xff)
#define AMDPK_SEMVER_PATCH(v) ((v) & 0xffff)

/* Hardware implementation versions. */
#define AMDPK_HWVER_MAJOR(v)  (((v) >> 24) & 0xff)
#define AMDPK_HWVER_MINOR(v)  (((v) >> 16) & 0xff)
#define AMDPK_HWVER_SVN(v)    ((v) & 0xffff)

/* Maximum number of queues supported by the driver. */
#define MAX_QUEUES 4

/* Number of RQ memory addresses for each queue. */
#define MAX_RQMEM_PER_QUEUE 4

/* Wait attempts for HW to flush all requests before close. */
#define MAX_FLUSH_WAIT_ATTEMPTS 500

/* Bit 0 (0x1) is the Generation bit. */
#define CQ_GENERATION_BIT BIT(0)

/* Bit 1 (0x2) is set when completion is valid. */
#define CQ_COMPLETION_BIT BIT(1)

/* Maximal value of rq_entries is 512. There is 1 CQ of 4K bytes.
 * Each completion status is 8 Bytes. Only 4096 / 8 = 512 entries
 * are possible at any time.
 */
#define MAX_CQ_ENTRIES_ON_PAGE (PAGE_SIZE / 8)

/* Forward declaration */
struct amdpk_dev;
struct amdpk_user;

/* structure to hold completion queue information */
struct amdpk_cq {
	/* PKI device */
	struct amdpk_dev *pkdev;
	/* Base address of the completion queue */
	u32 *base;
	/* tail representing last completion */
	unsigned int tail;
	/* generation bit which toggles as per the device */
	unsigned int generation;
	/* size code as configured in REG_RQ_CFG_PAGE_SIZE */
	u16 szcode;
};

/* represents PKI work context */
struct amdpk_work {
	/* PKI device */
	struct amdpk_dev *pkdev;
	/* PKI user */
	struct amdpk_user *user;
	/* Completion queue */
	struct amdpk_cq pk_cq;
	/* Kthread work associated with the PKI work */
	struct kthread_work cq_work;
	/* Kthred worker to handle completions */
	struct kthread_worker *cq_wq;
	/* Associated queue ID */
	u16 qid;
};

/* AMD PKI device */
struct amdpk_dev {
	/* DRM device associated with PKI device */
	struct drm_device ddev;
	/* Core device */
	struct device *dev;
	/* PKI register space address */
	char __iomem *regs;
	/* PKI register space physical address */
	resource_size_t regsphys;
	/* Maximum queues supported by device. */
	u16 max_queues;
	/* Available queues */
	struct ida avail_queues;
	/* Total available queues */
	atomic_t avail_qdepth;
	/* List of all the AMD users */
	struct amdpk_user *users[MAX_QUEUES];
	/* PKI work for each queue */
	struct amdpk_work *work[MAX_QUEUES];
};

/* AMD PKI user */
struct amdpk_user {
	/* PKI device */
	struct amdpk_dev *pkdev;
	/* Indicates if user has been configured */
	bool configured;
	/* Queue ID allocated for the user */
	u16 qid;
	/* Number of pages allocated on request queue */
	u16 rq_pages;
	/* RQ entries reserved for this user */
	size_t rq_entries;
	/* DMA address for RQ pages */
	dma_addr_t physrq[MAX_RQMEM_PER_QUEUE];
	/* RQ pages addresses */
	u8 *rqmem[MAX_RQMEM_PER_QUEUE];
	/* DMA address for CQ page */
	dma_addr_t physcq;
	/* CQ page address */
	u8 *cqmem;
	/* DMA address for status page */
	dma_addr_t physst;
	/* Status page address */
	u8 *stmem;
	/* Eventfd context for each request */
	struct eventfd_ctx *evfd_ctx[MAX_PK_REQS];
};

#define to_amdpk_dev(dev) container_of(dev, struct amdpk_dev, ddev)
#define to_amdpk_work(work) container_of(work, struct amdpk_work, cq_work)

void amdpk_debugfs_init(struct amdpk_dev *pkdev);

static void __maybe_unused pk_wrreg(char __iomem *regs, int addr, u64 val)
{
	iowrite64(val, regs + addr);
}

static u64 pk_rdreg(char __iomem *regs, int addr)
{
	return ioread64(regs + addr);
}

#endif /* __AMDPK_DRV_H__ */
