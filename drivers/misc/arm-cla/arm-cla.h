/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Arm CLA driver - internal definitions
 *
 * Copyright 2026 Arm Limited.
 */
#ifndef _ARM_CLA_H_
#define _ARM_CLA_H_

#include <linux/device.h>
#include <linux/io.h>
#include <linux/types.h>

#include "arm-cla-regs.h"

/* Number of accelerators per CLA */
#define CLA_NUM_ACC		8
#define CLA_NUM_DATA_REGS	8
#define CLA_SRSTATE_LEN		8

struct cla_domain;

/**
 * struct cla_dev - CLA device
 *
 * Immutable state:
 * @cpu:		The CPU this CLA is attached to.
 * @regs:		Registers accessed by the kernel.
 * @dev:		The platform device.
 * @pfn:		Page of registers assigned to user.
 * @pg_offset:		Mmap offset of this device.
 * @domain:		The domain this CLA belongs to.
 */
struct cla_dev {
	unsigned int cpu;
	void __iomem *regs;
	struct device *dev;
	unsigned long pfn;
	unsigned long pg_offset;
	struct cla_domain *domain;
};

/**
 * struct cla_domain - Collection of cla_dev
 *
 * Immutable state:
 * @id:			Domain identifier, from FW or generated.
 * @pg_offset:		Mmap offset of the first device.
 * @nr_devs:		Number of devices in the domain.
 * @devs:		Devices.
 */
struct cla_domain {
	unsigned int id;
	unsigned long pg_offset;
	unsigned int nr_devs;
	struct cla_dev **devs;
};

extern struct xarray cla_domains;
extern unsigned int cla_nr_domains;
extern struct cla_dev **cla_lut_cpu;
extern struct cla_dev **cla_lut_pg;
extern unsigned int cla_nr_devs;

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

int cla_op_wait_lresp(struct cla_dev *dev, u64 *lresp);
int cla_op_reset(struct cla_dev *dev, unsigned int accid);
int cla_op_regread(struct cla_dev *dev, unsigned int accid, unsigned int regidx,
		   size_t nregs, u64 *regs);
int cla_op_regwrite(struct cla_dev *dev, unsigned int accid,
		    unsigned int regidx, size_t nregs, u64 *regs);

#endif /* _ARM_CLA_H_ */
