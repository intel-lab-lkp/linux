// SPDX-License-Identifier: GPL-2.0

#include <linux/debugfs.h>
#include <linux/bitfield.h>

#include "fsl-edma-common.h"

#define fsl_edma_debugfs_reg(reg, b, _s, __name)	\
do {	reg->name = __stringify(__name);		\
	reg->offset = offsetof(struct _s, __name);	\
	reg->size = sizeof(((struct _s *)0)->__name);	\
	reg->bigendian = b;				\
	reg++;						\
} while (0)

#define fsl_edma_debugfs_regv1(reg, edma, __name)	\
do {	reg->name = __stringify(__name);		\
	reg->offset = edma->regs.__name - edma->membase;\
	reg->bigendian = edma->big_endian;		\
	reg++;						\
} while (0)

static void fsl_edma_debufs_tcdreg(struct fsl_edma_chan *chan, struct dentry *dir)
{
	struct debugfs_regset *regset;
	struct debugfs_reg *reg;
	struct device *dev;
	int be;

	be = chan->edma->big_endian;

	dev = &chan->pdev->dev;

	regset = devm_kzalloc(dev, sizeof(*regset), GFP_KERNEL);
	if (!regset)
		return;

	regset->dev = dev;
	regset->base = chan->tcd;

	/* sizeof(struct fsl_edma_hw_tcd)/sizeof(u16) is enough for hold all registers */
	reg = devm_kcalloc(dev, sizeof(struct fsl_edma_hw_tcd)/sizeof(u16),
			   sizeof(*reg), GFP_KERNEL);

	if (!reg)
		return;

	regset->regs = reg;

	fsl_edma_debugfs_reg(reg, be, fsl_edma_hw_tcd, saddr);
	fsl_edma_debugfs_reg(reg, be, fsl_edma_hw_tcd, soff);
	fsl_edma_debugfs_reg(reg, be, fsl_edma_hw_tcd, attr);
	fsl_edma_debugfs_reg(reg, be, fsl_edma_hw_tcd, nbytes);
	fsl_edma_debugfs_reg(reg, be, fsl_edma_hw_tcd, slast);
	fsl_edma_debugfs_reg(reg, be, fsl_edma_hw_tcd, daddr);
	fsl_edma_debugfs_reg(reg, be, fsl_edma_hw_tcd, doff);
	fsl_edma_debugfs_reg(reg, be, fsl_edma_hw_tcd, citer);
	fsl_edma_debugfs_reg(reg, be, fsl_edma_hw_tcd, dlast_sga);
	fsl_edma_debugfs_reg(reg, be, fsl_edma_hw_tcd, csr);
	fsl_edma_debugfs_reg(reg, be, fsl_edma_hw_tcd, biter);

	regset->nregs = reg - regset->regs;

	debugfs_create_regset("tcd", 0444, dir, regset);
}

static void fsl_edma3_debufs_chan(struct fsl_edma_chan *chan, struct dentry *entry)
{
	struct debugfs_regset *regset;
	struct debugfs_reg *reg;
	struct device *dev;
	int be;

	be = chan->edma->big_endian;

	dev = &chan->pdev->dev;

	regset = devm_kzalloc(dev, sizeof(*regset), GFP_KERNEL);
	if (!regset)
		return;

	regset->dev = dev;

	reg = devm_kcalloc(dev, sizeof(struct fsl_edma3_ch_reg)/sizeof(u32),
			   sizeof(*reg), GFP_KERNEL);

	if (!reg)
		return;

	regset->base = chan->tcd;
	regset->base -= offsetof(struct fsl_edma3_ch_reg, tcd);

	regset->regs = reg;

	fsl_edma_debugfs_reg(reg, be, fsl_edma3_ch_reg, ch_csr);
	fsl_edma_debugfs_reg(reg, be, fsl_edma3_ch_reg, ch_es);
	fsl_edma_debugfs_reg(reg, be, fsl_edma3_ch_reg, ch_int);
	fsl_edma_debugfs_reg(reg, be, fsl_edma3_ch_reg, ch_sbr);
	fsl_edma_debugfs_reg(reg, be, fsl_edma3_ch_reg, ch_pri);
	fsl_edma_debugfs_reg(reg, be, fsl_edma3_ch_reg, ch_mux);
	fsl_edma_debugfs_reg(reg, be, fsl_edma3_ch_reg, ch_mattr);

	regset->nregs = reg - regset->regs;
	debugfs_create_regset("regs", 0444, entry, regset);

	fsl_edma_debufs_tcdreg(chan, entry);
}

static void fsl_edma3_debugfs_init(struct fsl_edma_engine *edma)
{
	struct fsl_edma_chan *chan;
	struct dentry *dir;
	int i;

	for (i = 0; i < edma->n_chans; i++) {
		if (edma->chan_masked & BIT(i))
			continue;

		chan = &edma->chans[i];
		dir = debugfs_create_dir(chan->chan_name, edma->dma_dev.dbg_dev_root);

		fsl_edma3_debufs_chan(chan, dir);
	}

}

static void fsl_edma_debugfs_init(struct fsl_edma_engine *edma)
{
	struct debugfs_regset *regset;
	struct fsl_edma_chan *chan;
	struct debugfs_reg *reg;
	struct dentry *dir;
	struct device *dev;
	int i;

	dev = edma->dma_dev.dev;

	regset = devm_kzalloc(dev, sizeof(*regset), GFP_KERNEL);
	if (!regset)
		return;

	regset->dev = dev;

	reg = devm_kcalloc(dev, sizeof(struct edma_regs)/sizeof(void *), sizeof(*reg), GFP_KERNEL);

	if (!reg)
		return;

	regset->regs = reg;
	regset->base = edma->membase;

	fsl_edma_debugfs_regv1(reg, edma, cr);
	fsl_edma_debugfs_regv1(reg, edma, es);
	fsl_edma_debugfs_regv1(reg, edma, erqh);
	fsl_edma_debugfs_regv1(reg, edma, erql);
	fsl_edma_debugfs_regv1(reg, edma, eeih);
	fsl_edma_debugfs_regv1(reg, edma, eeil);
	fsl_edma_debugfs_regv1(reg, edma, seei);
	fsl_edma_debugfs_regv1(reg, edma, ceei);
	fsl_edma_debugfs_regv1(reg, edma, serq);
	fsl_edma_debugfs_regv1(reg, edma, cerq);
	fsl_edma_debugfs_regv1(reg, edma, cint);
	fsl_edma_debugfs_regv1(reg, edma, cerr);
	fsl_edma_debugfs_regv1(reg, edma, ssrt);
	fsl_edma_debugfs_regv1(reg, edma, cdne);
	fsl_edma_debugfs_regv1(reg, edma, inth);
	fsl_edma_debugfs_regv1(reg, edma, errh);
	fsl_edma_debugfs_regv1(reg, edma, errl);

	regset->nregs = reg - regset->regs;

	debugfs_create_regset("regs", 0444, edma->dma_dev.dbg_dev_root, regset);

	for (i = 0; i < edma->n_chans; i++) {
		if (edma->chan_masked & BIT(i))
			continue;

		chan = &edma->chans[i];
		dir = debugfs_create_dir(chan->chan_name, edma->dma_dev.dbg_dev_root);

		fsl_edma_debufs_tcdreg(chan, dir);
	}
}

void fsl_edma_debugfs_on(struct fsl_edma_engine *edma)
{
	if (!debugfs_initialized())
		return;

	debugfs_create_bool("big_endian", 0444, edma->dma_dev.dbg_dev_root, &edma->big_endian);
	debugfs_create_x64("chan_mask", 0444, edma->dma_dev.dbg_dev_root, &edma->chan_masked);
	debugfs_create_x32("n_chans", 0444, edma->dma_dev.dbg_dev_root, &edma->n_chans);

	if (edma->drvdata->flags & FSL_EDMA_DRV_SPLIT_REG)
		fsl_edma3_debugfs_init(edma);
	else
		fsl_edma_debugfs_init(edma);
}


