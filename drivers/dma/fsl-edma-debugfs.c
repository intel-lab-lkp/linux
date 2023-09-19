#include <linux/debugfs.h>
#include <linux/bitfield.h>

#include "fsl-edma-common.h"

#define fsl_edma_debugfs_reg(reg, dir, __name)						\
((sizeof(reg->__name) == sizeof(u32)) ?							\
	debugfs_create_x32(__stringify(__name), 0644, dir, (u32 *)&reg->__name) :	\
	debugfs_create_x16(__stringify(__name), 0644, dir, (u16 *)&reg->__name)		\
)

#define fsl_edma_debugfs_regv1(reg, dir, __name)				\
	debugfs_create_x32(__stringify(__name), 0644, dir, reg.__name)

static void fsl_edma_debufs_tcdreg(struct fsl_edma_chan *chan, struct dentry *dir)
{
	fsl_edma_debugfs_reg(chan->tcd, dir, saddr);
	fsl_edma_debugfs_reg(chan->tcd, dir, soff);
	fsl_edma_debugfs_reg(chan->tcd, dir, attr);
	fsl_edma_debugfs_reg(chan->tcd, dir, nbytes);
	fsl_edma_debugfs_reg(chan->tcd, dir, slast);
	fsl_edma_debugfs_reg(chan->tcd, dir, daddr);
	fsl_edma_debugfs_reg(chan->tcd, dir, doff);
	fsl_edma_debugfs_reg(chan->tcd, dir, citer);
	fsl_edma_debugfs_reg(chan->tcd, dir, dlast_sga);
	fsl_edma_debugfs_reg(chan->tcd, dir, csr);
	fsl_edma_debugfs_reg(chan->tcd, dir, biter);
}

static void fsl_edma3_debufs_chan(struct fsl_edma_chan *chan, struct dentry *entry)
{
	struct fsl_edma3_ch_reg *reg;
	struct dentry *dir;

	reg = container_of(chan->tcd, struct fsl_edma3_ch_reg, tcd);
	fsl_edma_debugfs_reg(reg, entry, ch_csr);
	fsl_edma_debugfs_reg(reg, entry, ch_int);
	fsl_edma_debugfs_reg(reg, entry, ch_sbr);
	fsl_edma_debugfs_reg(reg, entry, ch_pri);
	fsl_edma_debugfs_reg(reg, entry, ch_mux);
	fsl_edma_debugfs_reg(reg, entry, ch_mattr);

	dir = debugfs_create_dir("tcd_regs", entry);

	fsl_edma_debufs_tcdreg(chan, dir);
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
	struct fsl_edma_chan *chan;
	struct dentry *dir;
	int i;

	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, cr);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, es);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, erqh);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, erql);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, eeih);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, eeil);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, seei);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, ceei);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, serq);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, cerq);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, cint);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, cerr);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, ssrt);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, cdne);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, inth);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, errh);
	fsl_edma_debugfs_regv1(edma->regs, edma->dma_dev.dbg_dev_root, errl);

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


