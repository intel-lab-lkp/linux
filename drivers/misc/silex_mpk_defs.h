// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2021 Silex Insight sa
 * Copyright (c) 2018-2021 Beerten Engineering scs
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 */

#ifndef __SILEX_MPK_REGS_H__
#define __SILEX_MPK_REGS_H__

/* Contains magic number 0x5113C5OC.
 * Used to validate access to the hardware registers
 */
#define REG_MAGIC (0x00)

/* Contains version of the hardware interface as semver
 * The semantic version : major 8 bits, minor 8 bits in little endian order.
 */
#define REG_SEMVER (0x08)

/* The number of request queues available in the hardware */
#define REQ_CFG_REQ_QUEUES_CNT 0x10

/* The maximum number of pending requests from all request queues combined. */
#define REQ_CFG_MAX_PENDING_REQ 0x18

/* The maximum number of pending requests in a single request queue. */
#define REQ_CFG_MAX_REQ_QUEUE_ENTRIES 0x0020

/* The first 16 bits give the amount of PK core instances with 64 multipliers.
 * The next 16 bits give the amount of PK core instances with 256 multipliers.
 */
#define REQ_CFG_PK_INST 0x28

/* Writing 0x1 puts all pkcore accelerators and scheduler in reset.
 * Writing 0x0 makes all pkcore accelerators and scheduler leave reset
 * and become operational.
 */
#define REG_PK_GLOBAL_STATE 0x38

/* The semantic version : major 8 bits, minor 8 bits,
 * scm id 16 bits in little endian order.
 */
#define REG_HW_VERSION (0x40)

/* Bitmask of which CQ interrupts are raised */
#define REG_PK_IRQ_STATUS 0x88

/* Bitmask of which CQ may trigger interrupts */
#define REG_IRQ_ENABLE 0x90

/* Bitmask of CQ interrupts to reset */
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

/* Index of the associated completion queue */
#define REG_RQ_CFG_CQID(qid) (0x00128 + (qid) * 0x80)

/* Bit field of pages where descriptor can write to.
 * When a bit is 1, a descriptor can write to the corresponding page.
 */
#define REG_RQ_CFG_PAGES_WREN(qid) (0x00138 + (qid) * 0x80)

/* Maximum number of entries which can be written into this request queue */
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

/* Interrupt number for this completion queue */
#define REG_CQ_CFG_IRQ_NR(qid) (0x1110 + (qid) * 0x80)

/* Control registers base address for the given request completion queue pair. */
#define REG_CTL_BASE(qid) (0x2000 + (qid) * 0x1000)

/* Count of how many requests are queued at a given time for this RQCQ.
 * When this count reaches 0, the resources of the request and
 * completion queues can be deleted.
 */
#define REG_CTL_PENDING_REQS  0x18

#define REG_CONTROL 0x00
#define REG_CONFIG 0x04
#define REG_DATACTRL 0x10
#define REG_ENTROPYIN 0x14
#define REG_PERSSTR 0x1C
#define REG_HWCONFIG 0x40

#define CONTROL_START 1
#define CONTROL_USE_ENTROPY_INPUT_REG BIT(2)
#define CONTROL_DO_INSTANTIATE BIT(5)
#define CONFIG_USE_AES_128 0
#define CONFIG_REQUEST_NB_BITS(bits) (((bits) - 1) << 16)

#define MPK_SEMVER_MAJOR(v) (((v) >> 24) & 0xff)
#define MPK_SEMVER_MINOR(v) (((v) >> 16) & 0xff)
#define MPK_SEMVER_PATCH(v) ((v) & 0xffff)
#define MPK_HWVER_MAJOR(v)  (((v) >> 24) & 0xff)
#define MPK_HWVER_MINOR(v)  (((v) >> 16) & 0xff)
#define MPK_HWVER_SVN(v)    ((v) & 0xffff)

#define MAX_FLUSH_WAIT_ATTEMPTS 500
#define MULTIPK_MAX_DEVICES 1
#define MAX_QUEUES 4
#define MAX_RQMEM_PER_QUEUE 4

struct multipk_dev;
struct multipk_user;

struct sx_pk_cq {
	struct multipk_dev *mpkdev;
	unsigned int generation;
	int szcode;
	u32 *base;
	unsigned int tail;
};

struct multipk_work {
	int qid;
	struct multipk_dev *mpkdev;
	struct multipk_user *user;
	struct sx_pk_cq pk_cq;
	struct kthread_work cq_work;
	struct kthread_worker *cq_wq;
};

struct multipk_dev {
	struct device *dev;
	char __iomem *regs;
	resource_size_t regsphys;
	char __iomem *dbrg_regs;
	resource_size_t dbrg_regsphys;
	struct cdev cdev;
	struct ida available_rqcq;
	atomic_t allowed_reqs;
	int max_queues;
	struct multipk_user **users;
	long ntfy_mask;
	/* Lock required between release and CQ handling */
	struct mutex lock[MAX_QUEUES];
	struct multipk_work work[MAX_QUEUES];
};

struct multipk_user {
	struct multipk_dev *mpkdev;
	dma_addr_t physrq[MAX_RQMEM_PER_QUEUE];
	char *rqmem[MAX_RQMEM_PER_QUEUE];
	dma_addr_t physcq;
	char *cqmem;
	dma_addr_t physsh;
	char *shmem;
	size_t rq_entries;
	int qid;
	int rq_pages;
	struct eventfd_ctx *evfd_ctx[MAX_PK_REQS];
};

#endif /* __SILEX_MPK_REGS_H__ */
