// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2014-2018 MediaTek Inc.

/*
 * Library for MediaTek External Interrupt Support
 *
 * Author: Maoguang Meng <maoguang.meng@mediatek.com>
 *	   Sean Wang <sean.wang@mediatek.com>
 *
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

#include "mtk-eint.h"

static struct mtk_eint *global_eintc;
struct mtk_eint_pin pin;

static const struct mtk_eint_regs mtk_generic_eint_regs = {
	.stat      = 0x000,
	.ack       = 0x040,
	.mask      = 0x080,
	.mask_set  = 0x0c0,
	.mask_clr  = 0x100,
	.sens      = 0x140,
	.sens_set  = 0x180,
	.sens_clr  = 0x1c0,
	.soft      = 0x200,
	.soft_set  = 0x240,
	.soft_clr  = 0x280,
	.pol       = 0x300,
	.pol_set   = 0x340,
	.pol_clr   = 0x380,
	.dom_en    = 0x400,
	.dbnc_ctrl = 0x500,
	.dbnc_set  = 0x600,
	.dbnc_clr  = 0x700,
	.event     = 0x800,
	.event_set = 0x840,
	.event_clr = 0x880,
	.raw_stat  = 0xa00,
};

const unsigned int debounce_time_mt2701[] = {
	500, 1000, 16000, 32000, 64000, 128000, 256000, 0
};
EXPORT_SYMBOL_GPL(debounce_time_mt2701);

const unsigned int debounce_time_mt6765[] = {
	125, 250, 500, 1000, 16000, 32000, 64000, 128000, 256000, 512000, 0
};
EXPORT_SYMBOL_GPL(debounce_time_mt6765);

const unsigned int debounce_time_mt6795[] = {
	500, 1000, 16000, 32000, 64000, 128000, 256000, 512000, 0
};
EXPORT_SYMBOL_GPL(debounce_time_mt6795);

/*
 * Return the iomem of specific register ofset and decode the coordinate
 * (instance, index) from global eint number.
 * If return NULL, then it must be either out-of-range or do-not-support.
 */
static void __iomem *mtk_eint_get_ofset(struct mtk_eint *eint,
					 unsigned int eint_num,
					 unsigned int ofset,
					 unsigned int *instance,
					 unsigned int *index)
{
	void __iomem *reg;

	if (eint_num >= eint->total_pin_number ||
	    !eint->pins[eint_num].enabled) {
		WARN_ON(1);
		return NULL;
	}

	*instance = eint->pins[eint_num].instance;
	*index = eint->pins[eint_num].index;
	reg = eint->instances[*instance].base + ofset + (*index / MAX_BIT * REG_OFSET);

	return reg;
}

/*
 * Generate helper function to access property register of a dedicate pin.
 */
#define DEFINE_EINT_GET_FUNCTION(_NAME, _OFSET) \
static unsigned int mtk_eint_get_##_NAME(struct mtk_eint *eint, \
				   unsigned int eint_num) \
{ \
	unsigned int instance, index; \
	void __iomem *reg = mtk_eint_get_ofset(eint, eint_num, \
						_OFSET, \
						&instance, &index); \
	unsigned int bit = BIT(index & 0x1f);\
\
	if (!reg) { \
		dev_err(eint->dev, "%s invalid eint_num %d\n", \
			__func__, eint_num); \
		return 0;\
	} \
\
	return !!(readl(reg) & bit); \
}

DEFINE_EINT_GET_FUNCTION(stat, eint->comp->regs->stat);
DEFINE_EINT_GET_FUNCTION(mask, eint->comp->regs->mask);
DEFINE_EINT_GET_FUNCTION(sens, eint->comp->regs->sens);
DEFINE_EINT_GET_FUNCTION(pol, eint->comp->regs->pol);
DEFINE_EINT_GET_FUNCTION(dom_en, eint->comp->regs->dom_en);
DEFINE_EINT_GET_FUNCTION(event, eint->comp->regs->event);
DEFINE_EINT_GET_FUNCTION(raw_stat, eint->comp->regs->raw_stat);

int dump_eint_pin_status(unsigned int eint_num)
{
       unsigned int stat, raw_stat, mask, sens, pol, dom_en, event;

       if (eint_num < 0 || eint_num > global_eintc->total_pin_number)
               return ENODEV;

       stat = mtk_eint_get_stat(global_eintc, eint_num);
       raw_stat = mtk_eint_get_raw_stat(global_eintc, eint_num);
       mask = mtk_eint_get_mask(global_eintc, eint_num);
       sens = mtk_eint_get_sens(global_eintc, eint_num);
       pol = mtk_eint_get_pol(global_eintc, eint_num);
       dom_en = mtk_eint_get_dom_en(global_eintc, eint_num);
       event = mtk_eint_get_event(global_eintc, eint_num);
       dev_info(global_eintc->dev, "%s eint_num:%u=stat:%u,raw:%u, \
		       mask:%u, sens:%u,pol:%u,dom_en:%u,event:%u\n",
		       __func__, eint_num, stat, raw_stat, mask, sens,
		       pol, dom_en, event);
       return 0;
}
EXPORT_SYMBOL_GPL(dump_eint_pin_status);

static unsigned int mtk_eint_can_en_debounce(struct mtk_eint *eint,
					     unsigned int eint_num)
{
	unsigned int sens;
	unsigned int instance, index;
	void __iomem *reg = mtk_eint_get_ofset(eint, eint_num,
						eint->comp->regs->sens,
						&instance, &index);
	unsigned int bit = BIT(index & 0x1f);

	if (!reg) {
		dev_err(eint->dev, "%s invalid eint_num %d\n",
			__func__, eint_num);
		return 0;
	}

	if (readl(reg) & bit)
		sens = MTK_EINT_LEVEL_SENSITIVE;
	else
		sens = MTK_EINT_EDGE_SENSITIVE;

	if (eint->pins[eint_num].debounce &&
	    sens != MTK_EINT_EDGE_SENSITIVE)
		return 1;
	else
		return 0;
}

static int mtk_eint_flip_edge(struct mtk_eint *eint, int eint_num)
{
	int start_level, curr_level;
	unsigned int reg_ofset;
	unsigned int instance, index, mask, port;
	void __iomem *reg;

	reg = mtk_eint_get_ofset(eint, eint_num, MTK_EINT_NO_OFSET,
				  &instance, &index);

	if (!reg) {
		dev_err(eint->dev, "%s invalid eint_num %d\n",
			__func__, eint_num);
		return 0;
	}

	mask = BIT(index & 0x1f);
	port = index >> REG_GROUP;
	reg = eint->instances[instance].base + port * REG_OFSET;

	curr_level = eint->gpio_xlate->get_gpio_state(eint->pctl, eint_num);

	do {
		start_level = curr_level;
		if (start_level)
			reg_ofset = eint->comp->regs->pol_clr;
		else
			reg_ofset = eint->comp->regs->pol_set;

		writel(mask, reg + reg_ofset);

		curr_level = eint->gpio_xlate->get_gpio_state(eint->pctl,
							      eint_num);
	} while (start_level != curr_level);

	return start_level;
}

static void mtk_eint_mask(struct irq_data *d)
{
	struct mtk_eint *eint = irq_data_get_irq_chip_data(d);
	unsigned int instance, index;
	void __iomem *reg = mtk_eint_get_ofset(eint, d->hwirq,
						eint->comp->regs->mask_set,
						&instance, &index);
	u32 mask = BIT(index & 0x1f);

	if (!reg) {
		dev_err(eint->dev, "%s invalid eint_num %lu\n",
			__func__, d->hwirq);
		return;
	}

	eint->instances[instance].cur_mask[index >> REG_GROUP] &= ~mask;

	writel(mask, reg);
}

static void mtk_eint_unmask(struct irq_data *d)
{
	struct mtk_eint *eint = irq_data_get_irq_chip_data(d);
	unsigned int instance, index;
	void __iomem *reg = mtk_eint_get_ofset(eint, d->hwirq,
						eint->comp->regs->mask_clr,
						&instance, &index);
	u32 mask = BIT(index & 0x1f);

	if (!reg) {
		dev_err(eint->dev, "%s invalid eint_num %lu\n",
			__func__, d->hwirq);
		return;
	}

	eint->instances[instance].cur_mask[index >> REG_GROUP] |= mask;

	writel(mask, reg);

	if (eint->pins[d->hwirq].dual_edge)
		mtk_eint_flip_edge(eint, d->hwirq);
}

static void mtk_eint_ack(struct irq_data *d)
{
	struct mtk_eint *eint = irq_data_get_irq_chip_data(d);
	unsigned int instance, index;
	void __iomem *reg;
	unsigned int bit;

	if (eint->comp->ops.ack)
		eint->comp->ops.ack(d);
	else {
		reg = mtk_eint_get_ofset(eint, d->hwirq,
					  eint->comp->regs->ack,
					  &instance, &index);
		bit = BIT(index & 0x1f);
		if (!reg) {
			dev_err(eint->dev, "%s invalid eint_num %lu\n",
				__func__, d->hwirq);
			return;
		}

		writel(bit, reg);
	}
}

static void mtk_eint_soft_set(struct mtk_eint *eint,
				      unsigned int eint_num)
{
	unsigned int instance, index;
	void __iomem *reg = mtk_eint_get_ofset(eint, eint_num,
						eint->comp->regs->soft_set,
						&instance, &index);
	unsigned int bit = BIT(index & 0x1f);

	if (!reg) {
		dev_err(eint->dev, "%s invalid eint_num %d\n",
			__func__, eint_num);
		return;
	}

	writel(bit, reg);
}

static void mtk_eint_soft_clr(struct mtk_eint *eint,
				      unsigned int eint_num)
{
	unsigned int instance, index;
	void __iomem *reg = mtk_eint_get_ofset(eint, eint_num,
						eint->comp->regs->soft_clr,
						&instance, &index);
	unsigned int bit = BIT(index & 0x1f);

	if (!reg) {
		dev_err(eint->dev, "%s invalid eint_num %d\n",
			__func__, eint_num);
		return;
	}

	writel(bit, reg);
}

static int mtk_eint_set_type(struct irq_data *d, unsigned int type)
{
	struct mtk_eint *eint = irq_data_get_irq_chip_data(d);
	u32 mask;
	unsigned int instance, index;
	void __iomem *reg;

	if (((type & IRQ_TYPE_EDGE_BOTH) && (type & IRQ_TYPE_LEVEL_MASK)) ||
	    ((type & IRQ_TYPE_LEVEL_MASK) == IRQ_TYPE_LEVEL_MASK)) {
		dev_err(eint->dev,
			"Can't configure IRQ%d (EINT%lu) for type 0x%X\n",
			d->irq, d->hwirq, type);
		return -EINVAL;
	}

	if ((type & IRQ_TYPE_EDGE_BOTH) == IRQ_TYPE_EDGE_BOTH)
		eint->pins[d->hwirq].dual_edge = 1;
	else
		eint->pins[d->hwirq].dual_edge = 0;

	if (type & (IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_EDGE_FALLING))
		reg = mtk_eint_get_ofset(eint, d->hwirq,
					  eint->comp->regs->pol_clr,
					  &instance, &index);
	else
		reg = mtk_eint_get_ofset(eint, d->hwirq,
					  eint->comp->regs->pol_set,
					  &instance, &index);

	mask = BIT(index & 0x1f);
	writel(mask, reg);

	if (type & (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING))
		reg = mtk_eint_get_ofset(eint, d->hwirq,
					  eint->comp->regs->sens_clr,
					  &instance, &index);
	else
		reg = mtk_eint_get_ofset(eint, d->hwirq,
					  eint->comp->regs->sens_set,
					  &instance, &index);

	if (!reg) {
		dev_err(eint->dev, "%s invalid eint_num %lu\n",
			__func__, d->hwirq);
		return 0;
	}

	mask = BIT(index & 0x1f);
	writel(mask, reg);

	if (eint->pins[d->hwirq].dual_edge)
		mtk_eint_flip_edge(eint, d->hwirq);

	return 0;
}

static int mtk_eint_irq_set_wake(struct irq_data *d, unsigned int on)
{
	struct mtk_eint *eint = irq_data_get_irq_chip_data(d);
	unsigned int instance, index, shift, port;
	void __iomem *reg = mtk_eint_get_ofset(eint, d->hwirq,
						MTK_EINT_NO_OFSET,
						&instance, &index);

	if (!reg) {
		dev_err(eint->dev, "%s invalid eint_num %lu\n",
			__func__, d->hwirq);
		return 0;
	}

	shift = index & 0x1f;
	port = index >> REG_GROUP;

	if (on)
		eint->instances[instance].wake_mask[port] |= BIT(shift);
	else
		eint->instances[instance].wake_mask[port] &= ~BIT(shift);

	return 0;
}

static int mtk_eint_irq_request_resources(struct irq_data *d)
{
	struct mtk_eint *eint = irq_data_get_irq_chip_data(d);
	struct gpio_chip *gpio_c;
	unsigned int gpio_n;
	int err;

	err = eint->gpio_xlate->get_gpio_n(eint->pctl, d->hwirq,
					   &gpio_n, &gpio_c);
	if (err < 0) {
		dev_err(eint->dev, "Can not find pin\n");
		return err;
	}

	err = gpiochip_lock_as_irq(gpio_c, gpio_n);
	if (err < 0) {
		dev_err(eint->dev, "unable to lock HW IRQ %lu for IRQ\n",
			irqd_to_hwirq(d));
		return err;
	}

	err = eint->gpio_xlate->set_gpio_as_eint(eint->pctl, d->hwirq);
	if (err < 0) {
		dev_err(eint->dev, "Can not eint mode\n");
		return err;
	}

	return 0;
}

static void mtk_eint_irq_release_resources(struct irq_data *d)
{
	struct mtk_eint *eint = irq_data_get_irq_chip_data(d);
	struct gpio_chip *gpio_c;
	unsigned int gpio_n;

	eint->gpio_xlate->get_gpio_n(eint->pctl, d->hwirq, &gpio_n,
				     &gpio_c);

	gpiochip_unlock_as_irq(gpio_c, gpio_n);
}

static struct irq_chip mtk_eint_irq_chip = {
	.name = "mtk-eint",
	.irq_disable = mtk_eint_mask,
	.irq_mask = mtk_eint_mask,
	.irq_unmask = mtk_eint_unmask,
	.irq_ack = mtk_eint_ack,
	.irq_set_type = mtk_eint_set_type,
	.irq_set_wake = mtk_eint_irq_set_wake,
	.irq_request_resources = mtk_eint_irq_request_resources,
	.irq_release_resources = mtk_eint_irq_release_resources,
};

/*
 * Configure all EINT pins as domain 0, which only belongs to AP.
 */
static unsigned int mtk_eint_hw_init(struct mtk_eint *eint)
{
	void __iomem *reg,*eevt_clr;
	unsigned int i, j;

	for (i = 0; i < eint->instance_number; i++) {
		reg = eint->instances[i].base + eint->comp->regs->dom_en;
		eevt_clr = eint->instances[i].base + eint->comp->regs->event_clr;
		for (j = 0; j < eint->instances[i].number; j += MAX_BIT, reg += REG_OFSET, eevt_clr += REG_OFSET) {
			writel(REG_VAL, reg);
			writel(REG_VAL, eevt_clr);
		}
	}

	return 0;
}

static inline void
mtk_eint_debounce_process(struct mtk_eint *eint, int eint_num)
{
	unsigned int rst, ctrl_ofset;
	unsigned int bit, dbnc;
	unsigned int instance, index;
	void __iomem *reg;

	reg = mtk_eint_get_ofset(eint, eint_num, MTK_EINT_NO_OFSET,
				  &instance, &index);

	if (!reg) {
		dev_err(eint->dev, "%s invalid eint_num %d\n",
			__func__, eint_num);
		return;
	}

	ctrl_ofset = (index / REG_OFSET) * REG_OFSET + eint->comp->regs->dbnc_ctrl;
	dbnc = readl(eint->instances[instance].base + ctrl_ofset);
	bit = MTK_EINT_DBNC_SET_EN << ((index % REG_OFSET) * DB_GROUP);

	if ((bit & dbnc) > 0) {
		ctrl_ofset = (index / REG_OFSET) * REG_OFSET + eint->comp->regs->dbnc_set;
		rst = MTK_EINT_DBNC_RST_BIT << ((index % REG_OFSET) * DB_GROUP);
		writel(rst, eint->instances[instance].base + ctrl_ofset);
	}
}

static void mtk_eint_irq_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct mtk_eint *eint = irq_desc_get_handler_data(desc);
	unsigned int status, i, j;
	int shift, port, eint_num, virq;
	unsigned int dual_edge, start_level, curr_level;
	struct mtk_eint_instance eint_instance;
	void __iomem *addr;

	chained_irq_enter(chip, desc);

	for (i = 0; i < eint->instance_number; i++) {
		eint_instance = eint->instances[i];

		/* Iterate all pins by port */
		for (j = 0; j < eint_instance.number; j += MAX_BIT) {
			port = j >> REG_GROUP;
			status = readl(eint_instance.base + port * REG_OFSET +
				       eint->comp->regs->stat);
			while (status) {
				shift = __ffs(status);
				status &= ~BIT(shift);

				eint_num = eint->instances[i].pin_list[shift + j];
				virq = irq_find_mapping(eint->domain, eint_num);

				/*
				 * If we get an interrupt on pin that was only required
				 * for wake (but no real interrupt requested), mask the
				 * interrupt (as would mtk_eint_resume do anyway later
				 * in the resume sequence).
				 */
				if (eint->instances[i].wake_mask[port] & BIT(shift) &&
				    !(eint->instances[i].cur_mask[port] & BIT(shift))) {
					addr = eint_instance.base + port * REG_OFSET +
						eint->comp->regs->mask_set;
					writel_relaxed(BIT(shift), addr);
				}

				dual_edge = eint->pins[eint_num].dual_edge;
				if (dual_edge) {
					/*
					 * Clear soft-irq in case we raised it last
					 * time.
					 */
					mtk_eint_soft_clr(eint, eint_num);

					start_level =
					eint->gpio_xlate->get_gpio_state(eint->pctl,
									 eint_num);
				}

				generic_handle_irq(virq);

				if (dual_edge) {
					curr_level = mtk_eint_flip_edge(eint, eint_num);

					/*
					 * If level changed, we might lost one edge
					 * interrupt, raised it through soft-irq.
					 */
					if (start_level != curr_level)
						mtk_eint_soft_set(eint, eint_num);
				}

				if (eint->pins[eint_num].debounce)
					mtk_eint_debounce_process(eint, eint_num);

			}
		}
	}
	chained_irq_exit(chip, desc);
}

int mtk_eint_do_suspend(struct mtk_eint *eint)
{
	unsigned int i, j, port;

	for (i = 0; i < eint->instance_number; i++) {
		struct mtk_eint_instance inst = eint->instances[i];

		for (j = 0; j < inst.number; j += MAX_BIT) {
			port = j >> REG_GROUP;
			writel_relaxed(~inst.wake_mask[port],
				       inst.base + port*REG_OFSET + eint->comp->regs->mask_set);
			writel_relaxed(inst.wake_mask[port],
				       inst.base + port*REG_OFSET + eint->comp->regs->mask_clr);
		}
	}
	dsb(sy);

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_eint_do_suspend);

int mtk_eint_do_resume(struct mtk_eint *eint)
{
	unsigned int i, j, port;

	for (i = 0; i < eint->instance_number; i++) {
		struct mtk_eint_instance inst = eint->instances[i];

		for (j = 0; j < inst.number; j += MAX_BIT) {
			port = j >> REG_GROUP;
			writel_relaxed(~inst.cur_mask[port],
				       inst.base + port*REG_OFSET + eint->comp->regs->mask_set);
			writel_relaxed(inst.cur_mask[port],
				       inst.base + port*REG_OFSET + eint->comp->regs->mask_clr);
		}
	}
	dsb(sy);

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_eint_do_resume);

int mtk_eint_set_debounce(struct mtk_eint *eint, unsigned long eint_num,
			  unsigned int debounce)
{
	int virq, eint_ofset;
	unsigned int set_ofset, bit, clr_bit, clr_ofset, rst, i, unmask,
		     dbnc;
	static const unsigned int debounce_time[] = { 156, 313, 625, 1250,
		20000, 40000, 80000, 160000, 320000, 640000 };
	struct irq_data *d;
	unsigned int instance, index;
	void __iomem *reg;

	/*
	 * Due to different number of bit field, we only decode
	 * the coordinate here, instead of get the VA.
	 */
	reg = mtk_eint_get_ofset(eint, eint_num, MTK_EINT_NO_OFSET,
				  &instance, &index);

	if (!reg) {
		dev_err(eint->dev, "%s invalid eint_num %lu\n",
			__func__, eint_num);
		return 0;
	}

	virq = irq_find_mapping(eint->domain, eint_num);
	eint_ofset = (index % REG_OFSET) * DB_GROUP;
	d = irq_get_irq_data(virq);

	reg = eint->instances[instance].base;
	set_ofset = (index / REG_OFSET) * REG_OFSET + eint->comp->regs->dbnc_set;
	clr_ofset = (index / REG_OFSET) * REG_OFSET + eint->comp->regs->dbnc_clr;

	if (!mtk_eint_can_en_debounce(eint, eint_num))
		return -EINVAL;

	/*
	 * Check eint number to avoid access out-of-range
	 */
	dbnc = ARRAY_SIZE(debounce_time) - 1;
	for (i = 0; i < ARRAY_SIZE(debounce_time); i++) {
		if (debounce <= debounce_time[i]) {
			dbnc = i;
			break;
		}
	}

	if (!mtk_eint_get_mask(eint, eint_num)) {
		mtk_eint_mask(d);
		unmask = 1;
	} else
		unmask = 0;

	clr_bit = 0xff << eint_ofset;
	writel(clr_bit, reg + clr_ofset);

	bit = ((dbnc << MTK_EINT_DBNC_SET_DBNC_BITS)
		| MTK_EINT_DBNC_SET_EN) << eint_ofset;
	rst = MTK_EINT_DBNC_RST_BIT << eint_ofset;
	writel(rst | bit, reg + set_ofset);

	/*
	 * Delay should be (8T @ 32k) from dbc rst to work correctly.
	 */
	if (unmask == 1)
		mtk_eint_unmask(d);

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_eint_set_debounce);

unsigned int mtk_eint_get_debounce_en(struct mtk_eint *eint,
				      unsigned int eint_num)
{
	unsigned int instance, index, bit;
	void __iomem *reg;

	reg = mtk_eint_get_ofset(eint, eint_num, MTK_EINT_NO_OFSET,
				  &instance, &index);

	if (!reg) {
		dev_err(eint->dev, "%s invalid eint_num %d\n",
			__func__, eint_num);
		return 0;
	}

	reg = eint->instances[instance].base +
		(index / REG_OFSET) * REG_OFSET + eint->comp->regs->dbnc_ctrl;

	bit = MTK_EINT_DBNC_SET_EN << ((index % REG_OFSET) * DB_GROUP);

	return (readl(reg) & bit) ? 1 : 0;
}

unsigned int mtk_eint_get_debounce_value(struct mtk_eint *eint,
					   unsigned int eint_num)
{
	unsigned int instance, index, mask, ofset;
	void __iomem *reg;

	reg = mtk_eint_get_ofset(eint, eint_num, MTK_EINT_NO_OFSET,
				  &instance, &index);

	if (!reg) {
		dev_err(eint->dev, "%s invalid eint_num %d\n",
			__func__, eint_num);
		return 0;
	}

	reg = eint->instances[instance].base +
		(index / REG_OFSET) * REG_OFSET + eint->comp->regs->dbnc_ctrl;

	ofset = MTK_EINT_DBNC_SET_DBNC_BITS + ((index % REG_OFSET) * DB_GROUP);
	mask = 0xf << ofset;

	return ((readl(reg) & mask) >> ofset);
}

int mtk_eint_find_irq(struct mtk_eint *eint, unsigned long eint_n)
{
	int irq;

	irq = irq_find_mapping(eint->domain, eint_n);
	if (!irq)
		return -EINVAL;

	return irq;
}
EXPORT_SYMBOL_GPL(mtk_eint_find_irq);

static const struct mtk_eint_compatible default_compat = {
	.regs = &mtk_generic_eint_regs,
};

static const struct of_device_id eint_compatible_ids[] = {
	{ }
};

int mtk_eint_do_init(struct mtk_eint *eint)
{
	int i, virq;
	unsigned int size;
	eint->instance_number = 1;
        dev_info(eint->dev, "%s eint in legacy mode, assign the matrix number to %u.\n",
			__func__, eint->instance_number);

	if (eint != NULL && eint->hw != NULL)
    		eint->total_pin_number = eint->hw->ap_num;
	else
		dev_info(eint->dev, "%s Error: eint or eint->hw is NULL\n.", __func__);

	for (i = 0; i < eint->total_pin_number; i++) {
		eint->pins[i].enabled = true;
		eint->pins[i].instance = 0;
		eint->pins[i].index = i;
		eint->pins[i].debounce =  (i < eint->hw->db_cnt) ? 1 : 0;

		eint->instances[0].pin_list[i] = i;
		eint->instances[0].number++;
	}

	for (i = 0; i < eint->instance_number; i++) {
		size = (eint->instances[i].number / MAX_BIT + 1) * sizeof(unsigned int);
		eint->instances[i].wake_mask =
			devm_kzalloc(eint->dev, size, GFP_KERNEL);
		eint->instances[i].cur_mask =
			devm_kzalloc(eint->dev, size, GFP_KERNEL);

		if (!eint->instances[i].wake_mask ||
		    !eint->instances[i].cur_mask)
			return -ENOMEM;
	}

	eint->domain = irq_domain_add_linear(eint->dev->of_node,
					     eint->total_pin_number,
					     &irq_domain_simple_ops, NULL);
	if (!eint->domain)
		return -ENOMEM;

	mtk_eint_hw_init(eint);
	for (i = 0; i < eint->total_pin_number; i++) {
		virq = irq_create_mapping(eint->domain, i);

		irq_set_chip_and_handler(virq, &mtk_eint_irq_chip,
					 handle_level_irq);
		irq_set_chip_data(virq, eint);
	}

	irq_set_chained_handler_and_data(eint->irq, mtk_eint_irq_handler,
					 eint);

	global_eintc = eint;

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_eint_do_init);

int mtk_eint_do_init_v2(struct mtk_eint *eint)
{
	int i, virq, matrix_number = 0;
	struct device_node *node;
	unsigned int ret, size, ofset;
	unsigned int id, inst, idx, support_deb;

	const phandle *ph;

	ph = of_get_property(eint->dev->of_node, "mediatek,eint", NULL);
	if (!ph) {
		dev_err(eint->dev, "Cannot find EINT phandle in PIO node.\n");
		return -ENODEV;
	}

	node = of_find_node_by_phandle(be32_to_cpup(ph));
	if (!node) {
		dev_err(eint->dev, "Cannot find EINT node by phandle.\n");
		return -ENODEV;
	}

	ret = of_property_read_u32(node, "mediatek,total-pin-number",
				   &eint->total_pin_number);
	if (ret) {
		dev_err(eint->dev,
		       "%s cannot read total-pin-number from device node.\n",
		       __func__);
		return -EINVAL;
	}

	dev_info(eint->dev, "%s eint total %u pins.\n", __func__,
		eint->total_pin_number);

	ret = of_property_read_u32(node, "mediatek,instance-num",
				   &eint->instance_number);
	if (ret)
		eint->instance_number = 1; // only 1 instance in legacy chip

	size = eint->instance_number * sizeof(struct mtk_eint_instance);
	eint->instances = devm_kzalloc(eint->dev, size, GFP_KERNEL);
	if (!eint->instances)
		return -ENOMEM;

	size = eint->total_pin_number * sizeof(struct mtk_eint_pin);
	eint->pins = devm_kzalloc(eint->dev, size, GFP_KERNEL);
	if (!eint->pins)
		return -ENOMEM;

	for (i = 0; i < eint->instance_number; i++) {
		ret = of_property_read_string_index(node, "reg-name", i,
						    &(eint->instances[i].name));
		if (ret) {
			dev_info(eint->dev,
				 "%s cannot read the name of instance %d.\n",
				 __func__, i);
		}

		eint->instances[i].base = of_iomap(node, i);
		if (!eint->instances[i].base)
			return -ENOMEM;
	}

	matrix_number = of_property_count_u32_elems(node, "mediatek,pins") / ARRAY_0;
	if (matrix_number < 0) {
		matrix_number = eint->total_pin_number;
		dev_info(eint->dev, "%s eint in legacy mode, assign the matrix number to %u.\n",
			 __func__, matrix_number);
	} else
		dev_info(eint->dev, "%s eint in new mode, assign the matrix number to %u.\n",
			 __func__, matrix_number);

	for (i = 0; i < matrix_number; i++) {
		ofset = i * REG_OFSET;

		ret = of_property_read_u32_index(node, "mediatek,pins",
					   ofset, &id);
		ret |= of_property_read_u32_index(node, "mediatek,pins",
					   ofset+FIRST, &inst);
		ret |= of_property_read_u32_index(node, "mediatek,pins",
					   ofset+SECOND, &idx);
		ret |= of_property_read_u32_index(node, "mediatek,pins",
					   ofset+THIRD, &support_deb);

		/* Legacy chip which no need to give coordinate list */
		if (ret) {
			id = i;
			inst = 0;
			idx = i;
			support_deb = (i < MAX_BIT) ? 1 : 0;
		}

		eint->pins[id].enabled = true;
		eint->pins[id].instance = inst;
		eint->pins[id].index = idx;
		eint->pins[id].debounce = support_deb;

		eint->instances[inst].pin_list[idx] = id;
		eint->instances[inst].number++;

#if defined(MTK_EINT_DEBUG)
		pin = eint->pins[id];
		dev_info(eint->dev,
			 "EINT%u in (%u-%u), su_deb = %u",
			 id,
			 pin.instance,
			 eint->instances[inst].number,
			 pin.debounce,
#endif
	}

	for (i = 0; i < eint->instance_number; i++) {
		size = (eint->instances[i].number / MAX_BIT + 1) * sizeof(unsigned int);
		eint->instances[i].wake_mask =
			devm_kzalloc(eint->dev, size, GFP_KERNEL);
		eint->instances[i].cur_mask =
			devm_kzalloc(eint->dev, size, GFP_KERNEL);

		if (!eint->instances[i].wake_mask ||
		    !eint->instances[i].cur_mask)
			return -ENOMEM;
	}

	eint->comp = &default_compat;

	eint->irq = irq_of_parse_and_map(node, 0);
	if (!eint->irq) {
		dev_err(eint->dev,
			"%s IRQ parse fail.\n", __func__);
		return -EINVAL;
	}

	eint->domain = irq_domain_add_linear(eint->dev->of_node,
					     eint->total_pin_number,
					     &irq_domain_simple_ops, NULL);
	if (!eint->domain)
		return -ENOMEM;

	mtk_eint_hw_init(eint);
	for (i = 0; i < eint->total_pin_number; i++) {
		virq = irq_create_mapping(eint->domain, i);

		irq_set_chip_and_handler(virq, &mtk_eint_irq_chip,
					 handle_level_irq);
		irq_set_chip_data(virq, eint);
	}

	irq_set_chained_handler_and_data(eint->irq, mtk_eint_irq_handler,
					 eint);

	global_eintc = eint;

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_eint_do_init_v2);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek EINT Driver");
