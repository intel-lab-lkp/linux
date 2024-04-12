// SPDX-License-Identifier: GPL-2.0

#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include "spacc_core.h"

/* Read the IRQ status register and process as needed */


void spacc_disable_int (struct spacc_device *spacc);

uint32_t spacc_process_irq(struct spacc_device *spacc)
{
	u32 temp;
	int x, cmd_max;
	unsigned long lock_flag;

	spin_lock_irqsave(&spacc->lock, lock_flag);

	temp = readl(spacc->regmap + SPACC_REG_IRQ_STAT);

	/* clear interrupt pin and run registered callback */
	if (temp & SPACC_IRQ_STAT_STAT) {
		SPACC_IRQ_STAT_CLEAR_STAT(spacc);
		if (spacc->op_mode == SPACC_OP_MODE_IRQ) {
			spacc->config.fifo_cnt <<= 2;
			if (spacc->config.fifo_cnt >=
					spacc->config.stat_fifo_depth)
				spacc->config.fifo_cnt =
					spacc->config.stat_fifo_depth;

			/* update fifo count to allow more stati to pile up*/
			spacc_irq_stat_enable(spacc, spacc->config.fifo_cnt);
			 /* reenable CMD0 empty interrupt*/
			spacc_irq_cmdx_enable(spacc, 0, 0);
		} else if (spacc->op_mode == SPACC_OP_MODE_WD) {
		}
		if (spacc->irq_cb_stat)
			spacc->irq_cb_stat(spacc);
	}

	/* Watchdog IRQ */
	if (spacc->op_mode == SPACC_OP_MODE_WD) {
		if (temp & SPACC_IRQ_STAT_STAT_WD) {
			if (++spacc->wdcnt == SPACC_WD_LIMIT) {
				/* this happens when you get too many IRQs that
				 *  go unanswered
				 */
				spacc_irq_stat_wd_disable(spacc);
				 /* we set the STAT CNT to 1 so that every job
				  * generates an IRQ now
				  */
				spacc_irq_stat_enable(spacc, 1);
				spacc->op_mode = SPACC_OP_MODE_IRQ;
			} else if (spacc->config.wd_timer < (0xFFFFFFUL >> 4)) {
				/* if the timer isn't too high lets bump it up
				 * a bit so as to give the IRQ a chance to
				 * reply
				 */
				spacc_set_wd_count(spacc,
						   spacc->config.wd_timer << 4);
			}

			SPACC_IRQ_STAT_CLEAR_STAT_WD(spacc);
			if (spacc->irq_cb_stat_wd)
				spacc->irq_cb_stat_wd(spacc);
		}
	}

	if (spacc->op_mode == SPACC_OP_MODE_IRQ) {
		cmd_max = (spacc->config.is_qos ? SPACC_CMDX_MAX_QOS :
				SPACC_CMDX_MAX);
		for (x = 0; x < cmd_max; x++) {
			if (temp & SPACC_IRQ_STAT_CMDX(x)) {
				spacc->config.fifo_cnt = 1;
				/* disable CMD0 interrupt since STAT=1 */
				spacc_irq_cmdx_disable(spacc, x);
				spacc_irq_stat_enable(spacc,
						      spacc->config.fifo_cnt);

				SPACC_IRQ_STAT_CLEAR_CMDX(spacc, x);
				/* run registered callback */
				if (spacc->irq_cb_cmdx)
					spacc->irq_cb_cmdx(spacc, x);
			}
		}
	}

	spin_unlock_irqrestore(&spacc->lock, lock_flag);

	return temp;
}

void spacc_set_wd_count(struct spacc_device *spacc, uint32_t val)
{
	writel(val, spacc->regmap + SPACC_REG_STAT_WD_CTRL);
}

/* cmdx and cmdx_cnt depend on HW config */
/* cmdx can be 0, 1 or 2 */
/* cmdx_cnt must be 2^6 or less */
void spacc_irq_cmdx_enable(struct spacc_device *spacc, int cmdx, int cmdx_cnt)
{
	u32 temp;

	/* read the reg, clear the bit range and set the new value */
	temp = readl(spacc->regmap + SPACC_REG_IRQ_CTRL) &
		(~SPACC_IRQ_CTRL_CMDX_CNT_MASK(cmdx));
	temp |= SPACC_IRQ_CTRL_CMDX_CNT_SET(cmdx, cmdx_cnt);
	writel(temp | SPACC_IRQ_CTRL_CMDX_CNT_SET(cmdx, cmdx_cnt),
			spacc->regmap + SPACC_REG_IRQ_CTRL);

	writel(readl(spacc->regmap + SPACC_REG_IRQ_EN) |
				SPACC_IRQ_EN_CMD(cmdx),
				spacc->regmap + SPACC_REG_IRQ_EN);
}

void spacc_irq_cmdx_disable(struct spacc_device *spacc, int cmdx)
{
	writel(readl(spacc->regmap + SPACC_REG_IRQ_EN) &
			(~SPACC_IRQ_EN_CMD(cmdx)),
			spacc->regmap + SPACC_REG_IRQ_EN);
}

void spacc_irq_stat_enable(struct spacc_device *spacc, int stat_cnt)
{
	u32 temp;

	temp = readl(spacc->regmap + SPACC_REG_IRQ_CTRL);
	if (spacc->config.is_qos) {
		temp &= (~SPACC_IRQ_CTRL_STAT_CNT_MASK_QOS);
		temp |= SPACC_IRQ_CTRL_STAT_CNT_SET_QOS(stat_cnt);
	} else {
		temp &= (~SPACC_IRQ_CTRL_STAT_CNT_MASK);
		temp |= SPACC_IRQ_CTRL_STAT_CNT_SET(stat_cnt);
	}

	writel(temp, spacc->regmap + SPACC_REG_IRQ_CTRL);
	writel(readl(spacc->regmap + SPACC_REG_IRQ_EN) |
				SPACC_IRQ_EN_STAT,
				spacc->regmap + SPACC_REG_IRQ_EN);
}

void spacc_irq_stat_disable(struct spacc_device *spacc)
{
	writel(readl(spacc->regmap + SPACC_REG_IRQ_EN) &
				(~SPACC_IRQ_EN_STAT),
				spacc->regmap + SPACC_REG_IRQ_EN);
}

void spacc_irq_stat_wd_enable(struct spacc_device *spacc)
{
	writel(readl(spacc->regmap + SPACC_REG_IRQ_EN) |
				SPACC_IRQ_EN_STAT_WD,
				spacc->regmap + SPACC_REG_IRQ_EN);
}

void spacc_irq_stat_wd_disable(struct spacc_device *spacc)
{
	writel(readl(spacc->regmap + SPACC_REG_IRQ_EN) &
				(~SPACC_IRQ_EN_STAT_WD),
				spacc->regmap + SPACC_REG_IRQ_EN);
}

void spacc_irq_glbl_enable(struct spacc_device *spacc)
{
	writel(readl(spacc->regmap + SPACC_REG_IRQ_EN) |
				SPACC_IRQ_EN_GLBL,
				spacc->regmap + SPACC_REG_IRQ_EN);
}

void spacc_irq_glbl_disable(struct spacc_device *spacc)
{
	writel(readl(spacc->regmap + SPACC_REG_IRQ_EN) &
				(~SPACC_IRQ_EN_GLBL),
				spacc->regmap + SPACC_REG_IRQ_EN);
}

void spacc_disable_int (struct spacc_device *spacc)
{
	writel(0, spacc->regmap + SPACC_REG_IRQ_EN);
}

/* a function to run callbacks in the IRQ handler */
irqreturn_t spacc_irq_handler(int irq, void *dev)
{
	struct spacc_priv *priv =
		platform_get_drvdata(to_platform_device(dev));
	struct spacc_device *spacc = &priv->spacc;

	if (spacc->config.oldtimer != spacc->config.timer) {
		spacc_set_wd_count(&priv->spacc,
				priv->spacc.config.wd_timer =
							spacc->config.timer);

		spacc->config.oldtimer = spacc->config.timer;
	}

	/* check irq flags and process as required */
	if (!spacc_process_irq(spacc))
		return IRQ_NONE;

	return IRQ_HANDLED;
}
