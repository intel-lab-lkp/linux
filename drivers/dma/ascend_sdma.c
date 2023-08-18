// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2019-2022 HiSilicon Limited. */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dmaengine.h>
#include <linux/slab.h>
#include "virt-dma.h"
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/iommu.h>

#define SDMA_DEVICE_NAME "sdma"

/* SDMA_CH_REGS */
#define SDMAM_CH_CTRL_REG		0x0000
#define SDMAM_CH_IIDR_REG		0x0004
#define SDMAM_CH_TYPER_REG		0x0008
#define SDMAM_CH_BYPASS_CTRL_REG	0x0014

#define SDMAM_IRQ_STATUS_REG		0x000c
#define SDMAM_IRQ_CTRL_REG		0x0010
#define SDMAM_IRQ_IOC_MASK		(1U << 16)
#define SDMAM_IRQ_IOE_MASK		(1U << 17)
#define SDMAM_IRQ_ERR_MASK		(0xFFU << 20)

#define SDMAM_CH_SQBASER_L_REG		0x0040
#define SDMAM_CH_SQBASER_H_REG		0x0044
#define SDMAM_CH_SQ_ATTR_REG		0x0048
#define SDMAM_CH_SQTDBR_REG		0x004c
#define SDMAM_CH_SQHDBR_REG		0x0050

#define SDMAM_CH_CQBASER_L_REG		0x0080
#define SDMAM_CH_CQBASER_H_REG		0x0084
#define SDMAM_CH_CQ_ATTR_REG		0X0088
#define SDMAM_CH_CQTDBR_REG		0x008c
#define SDMAM_CH_CQHDBR_REG		0x0090

/* SDMA_COMMON_REGS */
#define SDMA_COMMON_DMA_AXUSER_REG0	0x0FE0
#define SDMA_COMMON_DMA_AXUSER_REG1	0x0FE4
#define SDMA_COMMON_DMA_AXUSER_REG2	0x0FE8
#define SDMA_DFX_FEATURE_EN_REG		0x0FFC

#define SDMA_IOMEM_SIZE			0x10000
#define SDMA_CHANNEL_IOMEM_SIZE	0x1000

#define SDMA_SQ_ENTRY_SIZE		32UL
#define SDMA_CQ_ENTRY_SIZE		16UL

/* must be pow of 2 */
#define SDMA_SQ_LENGTH			(1U << 10)
#define SDMA_CQ_LENGTH			(1U << 10)
#define SDMA_SQ_SIZE			(SDMA_SQ_ENTRY_SIZE * SDMA_SQ_LENGTH)
#define SDMA_CQ_SIZE			(SDMA_CQ_ENTRY_SIZE * SDMA_CQ_LENGTH)

#define SDMA_MAX_COPY_SIZE		0x100000000UL
#define SDMA_COPY_SIZE_MASK		0xFFFFFFFFUL

#define SDMA_MAX_CHANNEL_NUM		64

/*
 * struct ascend_sdma_chip_data - Ascend chip specific data
 * @channel_iomem_size: Size of channel register space
 */
struct ascend_sdma_chip_data {
	unsigned int channel_iomem_size;
};

void set_sdma_channel_info(struct dma_chan *c, int pasid);

static u32 sdma_queue_count(u32 head, u32 tail, u32 len)
{
	return (tail - head) & (len - 1);
}

static int iommu_enabled;

struct sdma_sq_entry {
	u32 opcode          : 8;
	u32 ie              : 1;
	u32 sssv            : 1;
	u32 dssv            : 1;
	u32 sns             : 1;
	u32 dns             : 1;
	u32 qos             : 4;
	u32 sro             : 1;
	u32 dro             : 1;
	u32 partid          : 4;
	u32 mpamns          : 1;
	u32 reserved0       : 8;
	u32 src_streamid    : 16;
	u32 src_substreamid : 16;
	u32 dst_streamid    : 16;
	u32 dst_substreamid : 16;
	u32 length;
	union {
		u64 src_addr;
		struct {
			u32 src_addr_l;
			u32 src_addr_h;
		};
	};
	union {
		u64 dst_addr;
		struct {
			u32 dst_addr_l;
			u32 dst_addr_h;
		};
	};
};

struct sdma_cq_entry {
	u32 reserved1;
	u32 reserved2;
	u32 sqhd      : 16;
	u32 reserved3 : 16;
	u32 reserved4 : 16;
	u32 vld       : 1;
	u32 status    : 15;
};

/*
 * struct sdma_desc - sdma descriptor to manage transfer requests.
 */
struct sdma_desc {
	int pasid;
	struct virt_dma_desc vd;
	struct sdma_sq_entry entry;
};

/*
 * struct sdma_chan - sdma channel information
 */
struct sdma_chan {
	u16			idx;
	u16			cq_vld;

	u16			sq_head;
	u16			sq_tail;
	u16			cq_head;
	u16			cq_tail;

	/* must be page-aligned and continuous physical memory */
	struct sdma_sq_entry	*sq_base;
	struct sdma_cq_entry	*cq_base;

	/* used for discrete copy, pre-alloc the buffer, reserved for now */
	unsigned long		*src_addr;
	unsigned long		*dst_addr;
	unsigned long		*len;

	void __iomem *io_base;

	int id;
	struct virt_dma_chan vc;
	struct sdma_dev *sdev;

	struct sdma_desc *desc;
	char *name;
	int pasid;
};

#define SDMA_DEVICE_NAME_LENGTH_MAX 20
/*
 * struct sdma_dev - sdma controller information
 */
struct sdma_dev {
	struct	dma_device dma_dev;
	struct	device	*dev;
	void __iomem *io_base;

	u16		idx;
	u16		nr_channel;
	DECLARE_BITMAP(channel_map, SDMA_MAX_CHANNEL_NUM);
	u32		streamid;

	const struct ascend_sdma_chip_data *cdata;

	char	name[SDMA_DEVICE_NAME_LENGTH_MAX];
	struct	sdma_chan *channels;
};

static inline struct sdma_chan *to_sdma_chan(struct dma_chan *c)
{
	return container_of(c, struct sdma_chan, vc.chan);
}

static inline struct sdma_desc *to_sdma_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct sdma_desc, vd);
}

/* sdma supports sva transfer via iommu.
 * client must first set the pasid.
 */
void set_sdma_channel_info(struct dma_chan *c, int pasid)
{
	struct sdma_chan *sc = to_sdma_chan(c);

	sc->pasid = pasid;
}
EXPORT_SYMBOL_GPL(set_sdma_channel_info);

struct sdma_hardware_info {
	unsigned long	channel_map;
	u64		base_addr; /* physical address */
};

#define CHANNEL_MAP_PROP "hisi,ascend-sdma-channel-map"

static int of_sdma_collect_info(struct platform_device *pdev, struct sdma_hardware_info *info)
{
	int ret;
	u64 channel_map;
	struct resource res;
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;

	ret = of_property_read_u64(np, CHANNEL_MAP_PROP, &channel_map);
	if (ret < 0) {
		dev_err(dev, "get " CHANNEL_MAP_PROP " info from dtb failed, %d\n", ret);
		return ret;
	}
	info->channel_map = channel_map;

	ret = of_address_to_resource(np, 0, &res);
	if (ret < 0) {
		dev_err(dev, "get io_base info from dtb failed, %d\n", ret);
		return ret;
	}

	info->base_addr = res.start;
	if (resource_size(&res) != SDMA_IOMEM_SIZE)
		dev_warn(dev, "reg size %#llx check failed, use %#x\n",
				resource_size(&res), SDMA_IOMEM_SIZE);

	return 0;
}

static int sdma_channel_alloc_sq_cq(struct sdma_chan *pchan)
{
	unsigned long *buf;
	struct device *dev = pchan->sdev->dev;

	pchan->sq_base = (struct sdma_sq_entry *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
			get_order(SDMA_SQ_SIZE));
	if (!pchan->sq_base) {
		dev_err(dev, "channel%d: alloc sq_memory failed\n", pchan->idx);
		return -ENOMEM;
	}

	pchan->cq_base = (struct sdma_cq_entry *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
			get_order(SDMA_CQ_SIZE));
	if (!pchan->cq_base) {
		dev_err(dev, "channel%d: alloc cq_memory failed\n", pchan->idx);
		free_pages((unsigned long)pchan->sq_base, get_order(SDMA_SQ_SIZE));
		return -ENOMEM;
	}

	buf = vmalloc(sizeof(unsigned long) * SDMA_SQ_LENGTH * 3);
	if (!buf) {
		dev_err(dev, "channel%d: alloc user_buf failed\n", pchan->idx);
		free_pages((unsigned long)pchan->sq_base, get_order(SDMA_SQ_SIZE));
		free_pages((unsigned long)pchan->cq_base, get_order(SDMA_CQ_SIZE));
		return -ENOMEM;
	}
	pchan->src_addr = buf;
	pchan->dst_addr = buf + SDMA_SQ_LENGTH;
	pchan->len      = buf + SDMA_SQ_LENGTH * 2;

	return 0;
}

static void sdma_free_all_sq_cq(struct sdma_dev *sdev)
{
	int i;
	struct sdma_chan *pchan;

	for (i = sdev->nr_channel - 1; i >= 0; i--) {
		pchan = sdev->channels + i;
		free_pages((unsigned long)pchan->sq_base, get_order(SDMA_SQ_SIZE));
		free_pages((unsigned long)pchan->cq_base, get_order(SDMA_CQ_SIZE));
		vfree(pchan->src_addr);
	}
}

static void sdma_channel_set_val_mask_shift(struct sdma_chan *pchan,
		int reg, u32 val, u32 mask, u32 shift)
{
	u32 reg_val = readl(pchan->io_base + reg);

	reg_val = (reg_val & ~(mask << shift)) | ((val & mask) << shift);
	writel(reg_val, pchan->io_base + reg);
}

static u32 sdma_channel_get_val_mask_shift(struct sdma_chan *pchan,
		int reg, u32 mask, u32 shift)
{
	u32 reg_val = readl(pchan->io_base + reg);

	return (reg_val >> shift) & mask;
}

static void sdma_channel_set_pause(struct sdma_chan *pchan)
{
	sdma_channel_set_val_mask_shift(pchan, SDMAM_CH_CTRL_REG, 1, 1, 1);
}

static bool sdma_channel_is_paused(struct sdma_chan *pchan)
{
	return sdma_channel_get_val_mask_shift(pchan, SDMAM_CH_CTRL_REG, 0xF, 16) == 3;
}

static bool sdma_channel_is_idle(struct sdma_chan *pchan)
{
	return sdma_channel_get_val_mask_shift(pchan, SDMAM_CH_CTRL_REG, 0xF, 16) == 0;
}

static bool sdma_channel_is_quiescent(struct sdma_chan *pchan)
{
	return sdma_channel_get_val_mask_shift(pchan, SDMAM_CH_CTRL_REG, 1, 31) == 1;
}

static void sdma_channel_write_reset(struct sdma_chan *pchan)
{
	sdma_channel_set_val_mask_shift(pchan, SDMAM_CH_CTRL_REG, 1, 1, 3);
}

static void sdma_channel_enable(struct sdma_chan *pchan)
{
	sdma_channel_set_val_mask_shift(pchan, SDMAM_CH_CTRL_REG, 1, 1, 0);
}

static void sdma_channel_set_doorbell_mode(struct sdma_chan *pchan)
{
	sdma_channel_set_val_mask_shift(pchan, SDMAM_CH_CTRL_REG, 0, 1, 9);
}

static void sdma_channel_disable(struct sdma_chan *pchan)
{
	sdma_channel_set_val_mask_shift(pchan, SDMAM_CH_CTRL_REG, 0, 1, 0);
}

static void sdma_channel_set_sq_size(struct sdma_chan *pchan, u32 size)
{
	sdma_channel_set_val_mask_shift(pchan, SDMAM_CH_SQ_ATTR_REG, size, 0xFFFF, 0);
}

static void sdma_channel_set_cq_size(struct sdma_chan *pchan, u32 size)
{
	sdma_channel_set_val_mask_shift(pchan, SDMAM_CH_CQ_ATTR_REG, size, 0xFFFF, 0);
}

static void sdma_channel_set_sq_tail(struct sdma_chan *pchan, u32 val)
{
	sdma_channel_set_val_mask_shift(pchan, SDMAM_CH_SQTDBR_REG, val, 0xFFFF, 0);
}

static u32 sdma_channel_get_sq_head(struct sdma_chan *pchan)
{
	return sdma_channel_get_val_mask_shift(pchan, SDMAM_CH_SQHDBR_REG, 0xFFFF, 0);
}

static void sdma_channel_set_cq_head(struct sdma_chan *pchan, u32 val)
{
	sdma_channel_set_val_mask_shift(pchan, SDMAM_CH_CQHDBR_REG, val, 0xFFFF, 0);
}

static u32 sdma_channel_get_cq_tail(struct sdma_chan *pchan)
{
	return sdma_channel_get_val_mask_shift(pchan, SDMAM_CH_CQTDBR_REG, 0xFFFF, 0);
}

static void sdma_channel_init(struct sdma_chan *pchan)
{
	void __iomem *io_base = pchan->io_base;
	u64 sq_addr = virt_to_phys(pchan->sq_base);
	u64 cq_addr = virt_to_phys(pchan->cq_base);

	writel(sq_addr & 0xFFFFFFFF, io_base + SDMAM_CH_SQBASER_L_REG);
	writel(sq_addr >> 32, io_base + SDMAM_CH_SQBASER_H_REG);
	writel(cq_addr & 0xFFFFFFFF, io_base + SDMAM_CH_CQBASER_L_REG);
	writel(cq_addr >> 32, io_base + SDMAM_CH_CQBASER_H_REG);

	sdma_channel_set_sq_size(pchan, SDMA_SQ_LENGTH - 1);
	sdma_channel_set_cq_size(pchan, SDMA_CQ_LENGTH - 1);
	sdma_channel_set_sq_tail(pchan, 0);
	sdma_channel_set_cq_head(pchan, 0);

	pchan->cq_vld = 1;
	sdma_channel_set_doorbell_mode(pchan);
	sdma_channel_enable(pchan);
}

static void sdma_channel_reset(struct sdma_chan *pchan)
{
	int i = 0;
	struct device *dev = pchan->sdev->dev;

	sdma_channel_set_pause(pchan);
	while (!sdma_channel_is_paused(pchan))
		if (++i > 10) {
			dev_warn(dev, "the channel cannot get paused\n");
			break;
		}

	i = 0;
	while (!sdma_channel_is_quiescent(pchan))
		if (++i > 10) {
			dev_warn(dev, "the channel cannot get quiescent\n");
			break;
		}

	i = 0;
	sdma_channel_write_reset(pchan);
	while (!sdma_channel_is_idle(pchan))
		if (++i > 10) {
			dev_warn(dev, "the channel cannot get idle\n");
			break;
		}
	sdma_channel_disable(pchan);

	pchan->sq_head = pchan->sq_tail = pchan->cq_head = pchan->cq_tail = 0;
	sdma_channel_init(pchan);
}

static void sdma_destroy_channels(struct sdma_dev *sdev)
{
	sdma_free_all_sq_cq(sdev);
}

static void sdma_desc_free(struct virt_dma_desc *vd)
{
	kfree(to_sdma_desc(vd));
}

static int sdma_init_channels(struct sdma_dev *sdev, struct sdma_hardware_info *info)
{
	int ret = 0;
	int i, nr_channel;
	struct sdma_chan *pchan;
	struct device *dev = sdev->dev;

	nr_channel = bitmap_weight(&info->channel_map, BITS_PER_LONG);

	if (!nr_channel || nr_channel > SDMA_MAX_CHANNEL_NUM) {
		dev_err(dev, "channel count (%d) invalid\n", nr_channel);
		return -ENODEV;
	}

	sdev->channels = devm_kcalloc(dev, nr_channel, sizeof(*sdev->channels),
					GFP_KERNEL);
	if (!sdev->channels)
		return -ENOMEM;

	sdev->nr_channel = 0;
	for (i = 0; sdev->nr_channel < nr_channel; i++) {
		if (!(info->channel_map & (1UL << i)))
			continue;

		pchan = sdev->channels + sdev->nr_channel;
		pchan->idx = sdev->nr_channel;
		pchan->sdev = sdev;
		pchan->name = devm_kasprintf(dev, GFP_KERNEL, "%s chan%d",
				dev_name(dev), i);

		ret = sdma_channel_alloc_sq_cq(pchan);
		if (ret < 0)
			goto err_out;

		sdev->nr_channel++;
		pchan->io_base = sdev->io_base + i * sdev->cdata->channel_iomem_size;
		vchan_init(&pchan->vc, &sdev->dma_dev);
		pchan->vc.desc_free = sdma_desc_free;

		sdma_channel_disable(pchan);
		sdma_channel_init(pchan);

		dev_info(dev, "hardware channel%d probed, idx %d\n", i, pchan->idx);
	}

	bitmap_set(sdev->channel_map, 0, sdev->nr_channel);

	return 0;

err_out:
	sdma_destroy_channels(sdev);

	return ret;
}

static struct dma_async_tx_descriptor *sdma_prep_dma_memcpy(
		struct dma_chan *chan, dma_addr_t dst, dma_addr_t src,
		size_t len, unsigned long flags)
{
	struct sdma_chan *sc = to_sdma_chan(chan);
	struct sdma_desc *d;

	d = kzalloc(sizeof(*d), GFP_NOWAIT);
	if (!d)
		return NULL;

	if (sc->pasid > 0)
		d->pasid = sc->pasid;

	d->entry.src_addr = src;
	d->entry.dst_addr = dst;
	d->entry.length = len;

	return vchan_tx_prep(&sc->vc, &d->vd, flags);
}

static void sdma_error_handle(struct sdma_chan *sc)
{
	u32 cq_tail = sdma_channel_get_cq_tail(sc);

	if (cq_tail < sc->cq_head)
		sc->cq_vld ^= 1;
	sc->cq_head = sc->cq_tail = cq_tail;
	sc->sq_head = sdma_channel_get_sq_head(sc);
}

static int sdma_terminate_all(struct dma_chan *chan)
{
	sdma_error_handle(to_sdma_chan(chan));
	sdma_channel_reset(to_sdma_chan(chan));

	return 0;
}

static void sdma_synchronize(struct dma_chan *chan)
{
	struct sdma_chan *sc = to_sdma_chan(chan);

	vchan_synchronize(&sc->vc);
}

static enum dma_status sdma_tx_status(struct dma_chan *chan,
		dma_cookie_t cookie,
		struct dma_tx_state *txstate)
{
	u32 cq_head, cq_tail, cq_count;
	u32 irq_reg, ch_ctrl_reg;
	struct sdma_cq_entry *cq_entry;
	struct sdma_chan *sc = to_sdma_chan(chan);
	struct device *dev = sc->sdev->dev;
	enum dma_status ret = DMA_IN_PROGRESS;

	dsb(sy);
	irq_reg = readl(sc->io_base + SDMAM_IRQ_STATUS_REG);
	ch_ctrl_reg = readl(sc->io_base + SDMAM_CH_CTRL_REG);

	if (irq_reg & SDMAM_IRQ_IOC_MASK) {
		writel(irq_reg, sc->io_base + SDMAM_IRQ_STATUS_REG);

		cq_head = sc->cq_head;
		cq_tail = sdma_channel_get_cq_tail(sc);
		cq_count = sdma_queue_count(cq_head, cq_tail, SDMA_CQ_LENGTH);
		if (!cq_count) {
			dev_err(dev, "unexpected complete irq\n");
			ret = DMA_ERROR;
			goto out;
		}

		for (; cq_count; cq_count--) {
			cq_entry = sc->cq_base + cq_head;
			if (cq_entry->vld != sc->cq_vld || cq_entry->status) {
				dev_err(dev, "cq_entry invalid, vld: %u, cq_vld: %u, status: %u\n",
						cq_entry->vld, sc->cq_vld, cq_entry->status);
				ret = DMA_ERROR;
			}
			if (++cq_head == SDMA_CQ_LENGTH) {
				sc->cq_vld ^= 1;
				cq_head = 0;
			}
		}

		sc->cq_head = cq_head;
		sdma_channel_set_cq_head(sc, cq_head);
		sc->sq_head = sdma_channel_get_sq_head(sc);
		sc->cq_tail = cq_tail;

		if (ret != DMA_ERROR) {
			ret = DMA_COMPLETE;
			vchan_cookie_complete(&sc->desc->vd);
		}
	} else if (irq_reg & SDMAM_IRQ_IOE_MASK) {
		writel(irq_reg, sc->io_base + SDMAM_IRQ_STATUS_REG);
		dev_err(dev, "sdma ioe interrupt occur, status: %#x\n", irq_reg);
		sdma_error_handle(sc);

		ret = DMA_ERROR;
	}

out:
	return ret;
}

static void sdma_start_transfer(struct sdma_chan *pchan)
{
	u16 sq_tail = pchan->sq_tail;
	struct sdma_sq_entry *entry = pchan->sq_base + sq_tail;
	struct sdma_desc *desc;
	struct virt_dma_desc *vd;

	vd = vchan_next_desc(&pchan->vc);
	if (!vd) {
		pchan->desc = NULL;
		return;
	}
	list_del(&vd->node);
	desc = to_sdma_desc(vd);
	pchan->desc = desc;

	memcpy(entry, &desc->entry, sizeof(struct sdma_sq_entry));

	entry->src_streamid = pchan->sdev->streamid;
	entry->dst_streamid = pchan->sdev->streamid;

	entry->sns          = 1;
	entry->dns          = 1;
	entry->ie           = 0;
	entry->partid       = 0;
	entry->mpamns       = 1;
	if (pchan->pasid) {
		entry->sssv            = 1;
		entry->dssv            = 1;
		entry->src_substreamid = pchan->pasid;
		entry->dst_substreamid = pchan->pasid;
	} else {
		entry->sssv = 0;
		entry->dssv = 0;
	}
	sq_tail = (sq_tail + 1) & (SDMA_SQ_LENGTH - 1);
	entry->ie = 1;

	dmb(sy);
	sdma_channel_set_sq_tail(pchan, sq_tail);
	pchan->sq_tail = sq_tail;
}

static void sdma_issue_pending(struct dma_chan *chan)
{
	struct sdma_chan *sc = to_sdma_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&sc->vc.lock, flags);

	if (vchan_issue_pending(&sc->vc) && !sc->desc)
		sdma_start_transfer(sc);

	spin_unlock_irqrestore(&sc->vc.lock, flags);
}

static void sdma_free_chan_resources(struct dma_chan *chan)
{
	struct sdma_chan *sc = to_sdma_chan(chan);

	sc->desc = NULL;
	sc->pasid = 0;
}

#define SDMA_BUSWIDTHS 1024
static void sdma_init_dma_device(struct dma_device *dma_dev, struct device *dev)
{
	dma_cap_set(DMA_MEMCPY, dma_dev->cap_mask);

	dma_dev->device_issue_pending = sdma_issue_pending;
	dma_dev->device_tx_status = sdma_tx_status;
	dma_dev->device_terminate_all = sdma_terminate_all;
	dma_dev->device_synchronize = sdma_synchronize;
	dma_dev->device_free_chan_resources = sdma_free_chan_resources;
	dma_dev->device_prep_dma_memcpy = sdma_prep_dma_memcpy;

	dma_dev->src_addr_widths = SDMA_BUSWIDTHS;
	dma_dev->dst_addr_widths = SDMA_BUSWIDTHS;
	dma_dev->directions = BIT(DMA_MEM_TO_MEM);

	dma_dev->dev = dev;

	INIT_LIST_HEAD(&dma_dev->channels);
}

static void sdma_enable_iommu(struct device *dev)
{
	int ret;

	ret = iommu_dev_enable_feature(dev, IOMMU_DEV_FEAT_IOPF);
	if (ret < 0) {
		dev_warn(dev, "iommu failed to init iopf, err %d\n", ret);
		return;
	}

	ret = iommu_dev_enable_feature(dev, IOMMU_DEV_FEAT_SVA);
	if (ret < 0) {
		dev_warn(dev, "iommu failed to init sva, err %d\n", ret);
		iommu_dev_disable_feature(dev, IOMMU_DEV_FEAT_IOPF);
		return;
	}

	iommu_enabled = 1;
}

static void sdma_disable_iommu(struct device *dev)
{
	if (!iommu_enabled)
		return;

	iommu_dev_disable_feature(dev, IOMMU_DEV_FEAT_SVA);
	iommu_dev_disable_feature(dev, IOMMU_DEV_FEAT_IOPF);
	iommu_enabled = 0;
}

static int sdma_device_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct sdma_dev *sdev;
	struct sdma_hardware_info info;
	const struct ascend_sdma_chip_data *cdata;

	/* In case iommu uninitialized yet */
	if (!dev->bus->iommu_ops)
		return dev_err_probe(dev, -EPROBE_DEFER, "defer probe sdma device\n");

	cdata = of_device_get_match_data(dev);
	if (!cdata)
		return dev_err_probe(dev, -ENODEV, "device match data not found\n");

	sdev = devm_kzalloc(dev, sizeof(*sdev), GFP_KERNEL);
	if (!sdev)
		return dev_err_probe(dev, -ENOMEM, "alloc sdma_device failed\n");
	sdev->dev = dev;
	sdev->cdata = cdata;
	platform_set_drvdata(pdev, sdev);

	ret = of_sdma_collect_info(pdev, &info);
	if (ret < 0)
		return dev_err_probe(dev, ret, "collect device info failed\n");

	sdev->io_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sdev->io_base))
		return dev_err_probe(dev, PTR_ERR(sdev->io_base), "ioremap failed\n");

	/* Fill in dmaengine */
	sdma_init_dma_device(&sdev->dma_dev, dev);

	sdma_enable_iommu(dev);
	sdev->streamid = dev->iommu->fwspec->ids[0];
	snprintf(sdev->name, SDMA_DEVICE_NAME_LENGTH_MAX, "sdma%d", sdev->idx);

	ret = sdma_init_channels(sdev, &info);
	if (ret < 0) {
		dev_err_probe(dev, ret, "failed to initialize channels\n");
		goto disable_iommu;
	}

	ret = dma_async_device_register(&sdev->dma_dev);
	if (ret < 0) {
		dev_err_probe(dev, ret, "failed to register DMA engine\n");
		goto destroy_channels;
	}

	return 0;

destroy_channels:
	sdma_destroy_channels(sdev);
disable_iommu:
	sdma_disable_iommu(dev);
	return ret;
}

static int sdma_device_remove(struct platform_device *pdev)
{
	struct sdma_dev *sdev = dev_get_drvdata(&pdev->dev);

	dma_async_device_unregister(&sdev->dma_dev);
	sdma_disable_iommu(&pdev->dev);
	sdma_destroy_channels(sdev);

	return 0;
}

static const struct ascend_sdma_chip_data ascend910_chip_data = {
	.channel_iomem_size = 0x400,
};

static const struct ascend_sdma_chip_data ascend310_chip_data = {
	.channel_iomem_size = 0x1000,
};

static const struct of_device_id sdma_of_match[] = {
	{ .compatible = "hisi,ascend910-sdma", .data = &ascend910_chip_data },
	{ .compatible = "hisi,ascend310-sdma", .data = &ascend310_chip_data },
	{ }
};
MODULE_DEVICE_TABLE(of, sdma_of_match);

static struct platform_driver sdma_driver = {
	.probe    = sdma_device_probe,
	.remove   = sdma_device_remove,
	.driver   = {
		.name           = SDMA_DEVICE_NAME,
		.of_match_table = sdma_of_match,
	},
};

module_platform_driver(sdma_driver);

MODULE_DESCRIPTION("SDMA controller driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Wang Wensheng <wangwensheng4@huawei.com>");
MODULE_AUTHOR("Guo Mengqi <guomengqi3@huawei.com>");
