// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018,2021-2025 NXP
 * Copyright 2025 Linaro Limited
 *
 * Author: Daniel Lezcano <daniel.lezcano@linaro.org>
 *
 * NXP S32G System Timer Module counters:
 *
 *  STM supports commonly required system and application software
 *  timing functions. STM includes a 32-bit count-up timer and four
 *  32-bit compare channels with a separate interrupt source for each
 *  channel. The timer is driven by the STM module clock divided by an
 *  8-bit prescale value (1 to 256). It has ability to stop the timer
 *  in Debug mode
 *
 */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/counter.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define STM_CR(__base)		(__base)
#define STM_CR_TEN		BIT(0)
#define STM_CR_FRZ		BIT(1)
#define STM_CR_CPS_MASK         GENMASK(15, 8)

#define STM_CCR0(__base)	((__base) + 0x10)
#define STM_CCR_CEN		BIT(0)

#define STM_CIR0(__base)	((__base) + 0x14)
#define STM_CIR_CIF		BIT(0)

#define STM_CMP0(__base)	((__base) + 0x18)

#define STM_CNT(__base)		((__base) + 0x04)

#define STM_ENABLE_MASK	(STM_CR_FRZ | STM_CR_TEN)

struct nxp_stm_context {
	u32 counter;
	u8 prescaler;
	bool is_started;
};

struct nxp_stm_cnt {
	spinlock_t lock; /* Protects counter */
	void __iomem *base;
	u64 counter;
	struct nxp_stm_context context;
};

static void nxp_stm_cnt_enable(struct nxp_stm_cnt *stm_cnt)
{
	u32 reg;

	reg = readl(STM_CR(stm_cnt->base));

	reg |= STM_ENABLE_MASK;

	writel(reg, STM_CR(stm_cnt->base));
}

static void nxp_stm_cnt_disable(struct nxp_stm_cnt *stm_cnt)
{
	u32 reg;

	reg = readl(STM_CR(stm_cnt->base));

	reg &= ~STM_ENABLE_MASK;

	writel(reg, STM_CR(stm_cnt->base));
}

static void nxp_stm_cnt_ccr_disable(struct nxp_stm_cnt *stm_cnt)
{
	writel(0, STM_CCR0(stm_cnt->base));
}

static void nxp_stm_cnt_ccr_enable(struct nxp_stm_cnt *stm_cnt)
{
	writel(STM_CCR_CEN, STM_CCR0(stm_cnt->base));
}

static void nxp_stm_cnt_cfg_overflow(struct nxp_stm_cnt *stm_cnt)
{
	/*
	 * The STM does not have a dedicated interrupt when the
	 * counter wraps. We need to use the comparator to check if it
	 * wrapped or not.
	 */
	writel(0, STM_CMP0(stm_cnt->base));
}

static u32 nxp_stm_cnt_get_counter(struct nxp_stm_cnt *stm_cnt)
{
	return readl(STM_CNT(stm_cnt->base));
}

static void nxp_stm_cnt_set_counter(struct nxp_stm_cnt *stm_cnt, u32 counter)
{
	writel(counter, STM_CNT(stm_cnt->base));
}

static void nxp_stm_cnt_set_prescaler(struct nxp_stm_cnt *stm_cnt, u8 prescaler)
{
	u32 reg;

	reg = readl(STM_CR(stm_cnt->base));

	FIELD_MODIFY(STM_CR_CPS_MASK, &reg, prescaler);

	writel(reg, STM_CR(stm_cnt->base));
}

static u8 nxp_stm_cnt_get_prescaler(struct nxp_stm_cnt *stm_cnt)
{
	u32 reg = readl(STM_CR(stm_cnt->base));

	return FIELD_GET(STM_CR_CPS_MASK, reg);
}

static bool nxp_stm_cnt_is_started(struct nxp_stm_cnt *stm_cnt)
{
	u32 reg;

	reg = readl(STM_CR(stm_cnt->base));

	return !!FIELD_GET(STM_CR_TEN, reg);
}

static void nxp_stm_cnt_irq_ack(struct nxp_stm_cnt *stm_cnt)
{
	writel(STM_CIR_CIF, STM_CIR0(stm_cnt->base));
}

static irqreturn_t nxp_stm_cnt_irq(int irq, void *dev_id)
{
	struct counter_device *counter = dev_id;
	struct nxp_stm_cnt *stm_cnt = counter_priv(counter);

	nxp_stm_cnt_irq_ack(stm_cnt);

	counter_push_event(counter, COUNTER_EVENT_OVERFLOW, 0);

	spin_lock(&stm_cnt->lock);
	stm_cnt->counter += U32_MAX;
	spin_unlock(&stm_cnt->lock);

	return IRQ_HANDLED;
}

static void nxp_stm_cnt_start(struct nxp_stm_cnt *stm_cnt)
{
	nxp_stm_cnt_cfg_overflow(stm_cnt);
	nxp_stm_cnt_enable(stm_cnt);
	nxp_stm_cnt_ccr_enable(stm_cnt);
}

static void nxp_stm_cnt_stop(struct nxp_stm_cnt *stm_cnt)
{
	nxp_stm_cnt_disable(stm_cnt);
	nxp_stm_cnt_irq_ack(stm_cnt);
	nxp_stm_cnt_ccr_disable(stm_cnt);
}

static int nxp_stm_cnt_prescaler_read(struct counter_device *counter,
				      struct counter_count *count, u8 *val)
{
	struct nxp_stm_cnt *stm_cnt = counter_priv(counter);

	*val = nxp_stm_cnt_get_prescaler(stm_cnt);

	return 0;
}

static int nxp_stm_cnt_prescaler_write(struct counter_device *counter,
				       struct counter_count *count, u8 val)
{
	struct nxp_stm_cnt *stm_cnt = counter_priv(counter);

	nxp_stm_cnt_set_prescaler(stm_cnt, val);

	return 0;
}

static int nxp_stm_cnt_count_enable_write(struct counter_device *counter,
					  struct counter_count *count, u8 enable)
{
	struct nxp_stm_cnt *stm_cnt = counter_priv(counter);

	if (enable)
		nxp_stm_cnt_start(stm_cnt);
	else
		nxp_stm_cnt_stop(stm_cnt);

	return 0;
}

static int nxp_stm_cnt_count_enable_read(struct counter_device *counter,
					 struct counter_count *count, u8 *enable)
{
	struct nxp_stm_cnt *stm_cnt = counter_priv(counter);

	*enable = nxp_stm_cnt_is_started(stm_cnt);

	return 0;
}

static struct counter_comp stm_cnt_count_ext[] = {
	COUNTER_COMP_COUNT_U8("prescaler", nxp_stm_cnt_prescaler_read, nxp_stm_cnt_prescaler_write),
	COUNTER_COMP_ENABLE(nxp_stm_cnt_count_enable_read, nxp_stm_cnt_count_enable_write),
};

static int nxp_stm_cnt_action_read(struct counter_device *counter,
				   struct counter_count *count,
				   struct counter_synapse *synapse,
				   enum counter_synapse_action *action)
{
	*action = COUNTER_SYNAPSE_ACTION_RISING_EDGE;

	return 0;
}

static int nxp_stm_cnt_count_read(struct counter_device *dev,
				  struct counter_count *count, u64 *val)
{
	struct nxp_stm_cnt *stm_cnt = counter_priv(dev);
	unsigned long irqflags;

	spin_lock_irqsave(&stm_cnt->lock, irqflags);
	*val = stm_cnt->counter + nxp_stm_cnt_get_counter(stm_cnt);
	spin_unlock_irqrestore(&stm_cnt->lock, irqflags);

	return 0;
}

static int nxp_stm_cnt_count_write(struct counter_device *dev,
				   struct counter_count *count, u64 val)
{
	struct nxp_stm_cnt *stm_cnt = counter_priv(dev);
	unsigned long irqflags;

	spin_lock_irqsave(&stm_cnt->lock, irqflags);
	stm_cnt->counter = 0;
	nxp_stm_cnt_set_counter(stm_cnt, 0);
	spin_unlock_irqrestore(&stm_cnt->lock, irqflags);

	return 0;
}

static const enum counter_function nxp_stm_cnt_functions[] = {
	COUNTER_FUNCTION_INCREASE,
};

static int nxp_stm_cnt_function_read(struct counter_device *counter,
				     struct counter_count *count,
				     enum counter_function *function)
{
	*function = COUNTER_FUNCTION_INCREASE;

	return 0;
}

static int nxp_stm_cnt_watch_validate(struct counter_device *counter,
				      const struct counter_watch *watch)
{
	switch (watch->event) {
	case COUNTER_EVENT_OVERFLOW:
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct counter_ops nxp_stm_cnt_counter_ops = {
	.action_read = nxp_stm_cnt_action_read,
	.count_read  = nxp_stm_cnt_count_read,
	.count_write = nxp_stm_cnt_count_write,
	.function_read = nxp_stm_cnt_function_read,
	.watch_validate = nxp_stm_cnt_watch_validate,
};

static const enum counter_synapse_action nxp_stm_cnt_synapse_actions[] = {
	COUNTER_SYNAPSE_ACTION_RISING_EDGE,
};

static struct counter_signal nxp_stm_cnt_signals[] = {
	{
		.id = 0,
		.name = "Counter wrap signal"
	},
};

static struct counter_synapse nxp_stm_cnt_synapses[] = {
	{
		.actions_list = nxp_stm_cnt_synapse_actions,
		.num_actions = ARRAY_SIZE(nxp_stm_cnt_synapse_actions),
		.signal = &nxp_stm_cnt_signals[0],
	},
};

static struct counter_count nxp_stm_cnt_counts[] = {
	{
		.id = 0,
		.name = "System Timer Module Counter",
		.synapses = nxp_stm_cnt_synapses,
		.num_synapses = ARRAY_SIZE(nxp_stm_cnt_synapses),
		.ext = stm_cnt_count_ext,
		.num_ext = ARRAY_SIZE(stm_cnt_count_ext),
	},
};

static int nxp_stm_cnt_suspend(struct device *dev)
{
	struct nxp_stm_cnt *stm_cnt = dev_get_drvdata(dev);

	stm_cnt->context.is_started = nxp_stm_cnt_is_started(stm_cnt);

	if (stm_cnt->context.is_started) {
		nxp_stm_cnt_stop(stm_cnt);
		stm_cnt->context.prescaler = nxp_stm_cnt_get_prescaler(stm_cnt);
		stm_cnt->context.counter = nxp_stm_cnt_get_counter(stm_cnt);
	}

	return 0;
}

static int nxp_stm_cnt_resume(struct device *dev)
{
	struct nxp_stm_cnt *stm_cnt = dev_get_drvdata(dev);

	if (stm_cnt->context.is_started) {
		nxp_stm_cnt_set_counter(stm_cnt, stm_cnt->context.counter);
		nxp_stm_cnt_set_prescaler(stm_cnt, stm_cnt->context.prescaler);
		nxp_stm_cnt_start(stm_cnt);
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(nxp_stm_cnt_pm_ops, nxp_stm_cnt_suspend,
				nxp_stm_cnt_resume);

static int nxp_stm_cnt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct counter_device *counter;
	struct nxp_stm_cnt *stm_cnt;
	struct clk *clk;
	void __iomem *base;
	int irq, ret;

	base = devm_of_iomap(dev, np, 0, NULL);
	if (IS_ERR(base))
		return dev_err_probe(dev, PTR_ERR(base), "Failed to iomap %pOFn\n", np);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return dev_err_probe(dev, irq, "Failed to get IRQ\n");

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "Clock not found\n");

	counter = devm_counter_alloc(dev, sizeof(*stm_cnt));
	if (!counter)
		return -ENOMEM;

	stm_cnt = counter_priv(counter);

	stm_cnt->base = base;
	stm_cnt->counter = 0;
	spin_lock_init(&stm_cnt->lock);

	counter->name       = "stm_counter";
	counter->parent     = &pdev->dev;
	counter->ops        = &nxp_stm_cnt_counter_ops;
	counter->counts     = nxp_stm_cnt_counts;
	counter->num_counts = ARRAY_SIZE(nxp_stm_cnt_counts);

	ret = devm_request_irq(dev, irq, nxp_stm_cnt_irq, IRQF_TIMER | IRQF_NOBALANCING,
			       dev_name(&counter->dev), counter);
	if (ret)
		return dev_err_probe(dev, ret, "Unable to allocate interrupt line\n");

	ret = devm_counter_add(dev, counter);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register counter\n");

	platform_set_drvdata(pdev, stm_cnt);

	return 0;
}

static void nxp_stm_cnt_remove(struct platform_device *pdev)
{
	struct nxp_stm_cnt *stm_cnt = platform_get_drvdata(pdev);

	if (nxp_stm_cnt_is_started(stm_cnt))
		nxp_stm_cnt_stop(stm_cnt);
}

static const struct of_device_id nxp_stm_cnt_of_match[] = {
	{ .compatible = "nxp,s32g2-stm-cnt", },
	{ }
};
MODULE_DEVICE_TABLE(of, nxp_stm_cnt_of_match);

static struct platform_driver nxp_stm_cnt_driver = {
	.probe  = nxp_stm_cnt_probe,
	.remove = nxp_stm_cnt_remove,
	.driver = {
		.name           = "nxp-stm-cnt",
		.pm		= pm_sleep_ptr(&nxp_stm_cnt_pm_ops),
		.of_match_table = nxp_stm_cnt_of_match,
	},
};
module_platform_driver(nxp_stm_cnt_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Daniel Lezcano");
MODULE_DESCRIPTION("NXP System Timer Module counter driver");
MODULE_IMPORT_NS("COUNTER");
