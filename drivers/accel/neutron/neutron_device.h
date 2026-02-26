/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright 2025-2026 NXP */

#ifndef __NEUTRON_DEVICE_H__
#define __NEUTRON_DEVICE_H__

#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/bits.h>
#include <drm/drm_device.h>

struct clk_bulk_data;

#define NEUTRON_FIRMWARE_NAME		"NeutronFirmware.elf"

/* Register offsets */
#define NEUTRON_REG_RESETCTRL		0x00
#define NEUTRON_REG_STATUSERR		0x04
#define NEUTRON_REG_INTENA		0x08
#define NEUTRON_REG_INTCLR		0x0C
#define NEUTRON_REG_APPCTRL		0x200
#define NEUTRON_REG_APPSTATUS		0x204
#define NEUTRON_REG_BASEDDRL		0x208
#define NEUTRON_REG_BASEDDRH		0x20C
#define NEUTRON_REG_RINGCTRL		0x230
#define NEUTRON_REG_TAIL		0x238
#define NEUTRON_REG_HEAD		0x23C
#define NEUTRON_REG_MBOX0		0x240
#define NEUTRON_REG_MBOX1		0x244
#define NEUTRON_REG_MBOX2		0x248
#define NEUTRON_REG_MBOX3		0x24C
#define NEUTRON_REG_MBOX4		0x250
#define NEUTRON_REG_MBOX5		0x254
#define NEUTRON_REG_MBOX6		0x258
#define NEUTRON_REG_MBOX7		0x25C
#define NEUTRON_REG_BASEINOUTL		0x280
#define NEUTRON_REG_BASEINOUTH		0x284
#define NEUTRON_REG_BASESPILLL		0x288
#define NEUTRON_REG_BASESPILLH		0x28C

/* Register fields */
#define RESETCTRL_ZVRUN			BIT(0)

#define INTENA_INFDONE			BIT(1)

#define APPCTRL_MBWR_MASK		GENMASK(31, 16)
#define APPCTRL_MBWR_MAGIC		0xF807

#define APPSTATUS_INFDONE		BIT(0)
#define APPSTATUS_INFHALTED		BIT(1)
#define APPSTATUS_FAULTCAUSE_MASK	GENMASK(21, 16)
#define APPSTATUS_CLEAR_MASK		GENMASK(4, 0)

#define RINGCTRL_ADDR_MASK		GENMASK(16, 8)
#define RINGCTRL_SIZE_MASK		GENMASK(7, 0)
#define RINGCTRL_SIZE_MULT		256

/* Neutron device-side memory map */
#define NEUTRON_ITCM_DA			0x0
#define NEUTRON_DTCM_DA			0x40000
#define NEUTRON_DTCM_BANK1_OFFSET	0x4000

/* Driver flags */
#define NEUTRON_BOOTED			BIT(0)

/**
 * struct neutron_mem_region - Neutron memory region descriptor
 * @va: kernel virtual address of the memory region
 * @da: Device address of the memory region
 * @size: size of the memory region
 */
struct neutron_mem_region {
	void __iomem *va;
	u64 da;
	size_t size;
};

enum neutron_mem_id {
	NEUTRON_MEM_REGS = 0,
	NEUTRON_MEM_ITCM,
	NEUTRON_MEM_DTCM,
	NEUTRON_MEM_MAX
};

/**
 * struct neutron_device - Neutron device structure
 * @base: Base DRM device
 * @dev: Pointer to underlying device
 * @mem_regions: Array of memory region descriptors
 * @irq: IRQ number
 * @clks: Neutron clocks
 * @num_clks: Number of clocks
 * @flags: Software flags used by driver
 */
struct neutron_device {
	struct drm_device base;
	struct device *dev;

	struct neutron_mem_region mem_regions[NEUTRON_MEM_MAX];

	int irq;
	struct clk_bulk_data *clks;
	int num_clks;
	u32 flags;
};

#define to_neutron_device(drm) \
	container_of(drm, struct neutron_device, base)

#define NEUTRON_REG(ndev, name) \
	((ndev)->mem_regions[NEUTRON_MEM_REGS].va + NEUTRON_REG_##name)

int neutron_boot(struct neutron_device *ndev);
void neutron_shutdown(struct neutron_device *ndev);
void neutron_enable_irq(struct neutron_device *ndev);
void neutron_disable_irq(struct neutron_device *ndev);
void neutron_handle_irq(struct neutron_device *ndev);

#endif /* __NEUTRON_DEVICE_H__ */
