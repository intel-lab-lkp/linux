// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2016 Felix Fietkau <nbd@nbd.name>
 */

#include "mt76.h"
#include "dma.h"
#include "trace.h"

static u32 mt76_mmio_rr(struct mt76_dev *dev, u32 offset)
{
	u32 val;

	val = readl(dev->mmio.regs + offset);
	trace_reg_rr(dev, offset, val);

	return val;
}

static void mt76_mmio_wr(struct mt76_dev *dev, u32 offset, u32 val)
{
	trace_reg_wr(dev, offset, val);
	writel(val, dev->mmio.regs + offset);
}

static u32 mt76_mmio_rmw(struct mt76_dev *dev, u32 offset, u32 mask, u32 val)
{
	val |= mt76_mmio_rr(dev, offset) & ~mask;
	mt76_mmio_wr(dev, offset, val);
	return val;
}

static void mt76_mmio_write_copy_portable(void __iomem *dst,
					  const u8 *src, int len)
{
	__le32 val;
	int i = 0;

	for (i = 0; i < ALIGN(len, 4); i += 4) {
		memcpy(&val, src + i, sizeof(val));
		writel(cpu_to_le32(val), dst + i);
	}
}

static void mt76_mmio_write_copy(struct mt76_dev *dev, u32 offset,
				 const void *data, int len)
{
	if (IS_ENABLED(CONFIG_CPU_BIG_ENDIAN)) {
		mt76_mmio_write_copy_portable(dev->mmio.regs + offset, data,
					      len);
		return;
	}
	__iowrite32_copy(dev->mmio.regs + offset, data, DIV_ROUND_UP(len, 4));
}

static void mt76_mmio_read_copy_portable(u8 *dst,
					 const void __iomem *src, int len)
{
	u32 val;
	int i;

	for (i = 0; i < ALIGN(len, 4); i += 4) {
		val = le32_to_cpu(readl(src + i));
		memcpy(dst + i, &val, sizeof(val));
	}
}

static void mt76_mmio_read_copy(struct mt76_dev *dev, u32 offset,
				void *data, int len)
{
	if (IS_ENABLED(CONFIG_CPU_BIG_ENDIAN)) {
		mt76_mmio_read_copy_portable(data, dev->mmio.regs + offset,
					     len);
		return;
	}
	__ioread32_copy(data, dev->mmio.regs + offset, DIV_ROUND_UP(len, 4));
}

static int mt76_mmio_wr_rp(struct mt76_dev *dev, u32 base,
			   const struct mt76_reg_pair *data, int len)
{
	while (len > 0) {
		mt76_mmio_wr(dev, data->reg, data->value);
		data++;
		len--;
	}

	return 0;
}

static int mt76_mmio_rd_rp(struct mt76_dev *dev, u32 base,
			   struct mt76_reg_pair *data, int len)
{
	while (len > 0) {
		data->value = mt76_mmio_rr(dev, data->reg);
		data++;
		len--;
	}

	return 0;
}

void mt76_set_irq_mask(struct mt76_dev *dev, u32 addr,
		       u32 clear, u32 set)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->mmio.irq_lock, flags);
	dev->mmio.irqmask &= ~clear;
	dev->mmio.irqmask |= set;
	if (addr) {
		if (mtk_wed_device_active(&dev->mmio.wed))
			mtk_wed_device_irq_set_mask(&dev->mmio.wed,
						    dev->mmio.irqmask);
		else
			mt76_mmio_wr(dev, addr, dev->mmio.irqmask);
	}
	spin_unlock_irqrestore(&dev->mmio.irq_lock, flags);
}
EXPORT_SYMBOL_GPL(mt76_set_irq_mask);

void mt76_mmio_init(struct mt76_dev *dev, void __iomem *regs)
{
	static const struct mt76_bus_ops mt76_mmio_ops = {
		.rr = mt76_mmio_rr,
		.rmw = mt76_mmio_rmw,
		.wr = mt76_mmio_wr,
		.write_copy = mt76_mmio_write_copy,
		.read_copy = mt76_mmio_read_copy,
		.wr_rp = mt76_mmio_wr_rp,
		.rd_rp = mt76_mmio_rd_rp,
		.type = MT76_BUS_MMIO,
	};

	dev->bus = &mt76_mmio_ops;
	dev->mmio.regs = regs;

	spin_lock_init(&dev->mmio.irq_lock);
}
EXPORT_SYMBOL_GPL(mt76_mmio_init);
