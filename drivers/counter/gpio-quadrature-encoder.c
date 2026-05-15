// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO-based Quadrature Encoder Counter Driver
 *
 * Reads quadrature encoder signals (A, B, and optional Index) via GPIOs.
 * Supports X1, X2, X4 quadrature decoding and pulse-direction mode in
 * both increase and decrease orientation, and pure increase/decrease
 * pulse counters.
 *
 * Copyright (C) 2026 CMBlu Energy AG
 * Author: Wadim Mueller <wafgo01@gmail.com>
 */

#include <linux/counter.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/types.h>

enum gpio_qenc_signal_id {
	GPIO_QENC_SIGNAL_A = 0,
	GPIO_QENC_SIGNAL_B,
	GPIO_QENC_SIGNAL_INDEX,
};

struct gpio_qenc_priv {
	struct gpio_desc *gpio_a;
	struct gpio_desc *gpio_b;
	struct gpio_desc *gpio_index;

	int irq_a;
	int irq_b;
	int irq_index;

	/* Serialises ISR updates against sysfs read/write paths. */
	spinlock_t lock;

	u64 count;
	u64 ceiling;
	u64 preset;
	bool preset_enabled;
	enum counter_count_direction direction;
	enum counter_function function;

	int prev_a;
	int prev_b;

	struct counter_count cnts[1];
	struct counter_signal signals[3];
	struct counter_synapse synapses[3];
};

/*
 * Encode the four quadrature transitions in a single 4-bit state:
 *   bit3 = prev_a, bit2 = prev_b, bit1 = curr_a, bit0 = curr_b.
 *
 * Indexing the table with this value yields the signed delta for an
 * X4 decoder.  Illegal transitions (both inputs toggled at once)
 * remain 0 so the count is unchanged.
 */
#define CREATE_QE_STATE(prev_a, prev_b, curr_a, curr_b) \
	(((prev_a) << 3) | ((prev_b) << 2) | ((curr_a) << 1) | (curr_b))

static const s8 gpio_qenc_quad_x4_table[16] = {
	[CREATE_QE_STATE(0, 0, 0, 0)] =  0,
	[CREATE_QE_STATE(0, 0, 0, 1)] = -1,
	[CREATE_QE_STATE(0, 0, 1, 0)] =  1,
	[CREATE_QE_STATE(0, 0, 1, 1)] =  0,
	[CREATE_QE_STATE(0, 1, 0, 0)] =  1,
	[CREATE_QE_STATE(0, 1, 0, 1)] =  0,
	[CREATE_QE_STATE(0, 1, 1, 0)] =  0,
	[CREATE_QE_STATE(0, 1, 1, 1)] = -1,
	[CREATE_QE_STATE(1, 0, 0, 0)] = -1,
	[CREATE_QE_STATE(1, 0, 0, 1)] =  0,
	[CREATE_QE_STATE(1, 0, 1, 0)] =  0,
	[CREATE_QE_STATE(1, 0, 1, 1)] =  1,
	[CREATE_QE_STATE(1, 1, 0, 0)] =  0,
	[CREATE_QE_STATE(1, 1, 0, 1)] =  1,
	[CREATE_QE_STATE(1, 1, 1, 0)] = -1,
	[CREATE_QE_STATE(1, 1, 1, 1)] =  0,
};

static void gpio_qenc_update_count(struct gpio_qenc_priv *priv, int delta)
{
	if (delta > 0) {
		priv->direction = COUNTER_COUNT_DIRECTION_FORWARD;
		if (priv->count == priv->ceiling)
			return;
		priv->count++;
	} else if (delta < 0) {
		priv->direction = COUNTER_COUNT_DIRECTION_BACKWARD;
		if (priv->count == 0)
			return;
		priv->count--;
	}
}

static int gpio_qenc_a_delta(struct gpio_qenc_priv *priv, int a, int b,
			     int prev_a, int prev_b)
{
	int state = CREATE_QE_STATE(prev_a, prev_b, a, b);

	switch (priv->function) {
	case COUNTER_FUNCTION_QUADRATURE_X4:
		return gpio_qenc_quad_x4_table[state];

	case COUNTER_FUNCTION_QUADRATURE_X2_A:
		/* Both edges of A; sign comes from current A vs B. */
		return (a == b) ? -1 : 1;

	case COUNTER_FUNCTION_QUADRATURE_X1_A:
		/* Rising edge of A only. */
		if (!prev_a && a)
			return b ? -1 : 1;
		return 0;

	case COUNTER_FUNCTION_PULSE_DIRECTION:
		/* A is pulse, B is direction. */
		if (!prev_a && a)
			return b ? -1 : 1;
		return 0;

	case COUNTER_FUNCTION_INCREASE:
		if (!prev_a && a)
			return 1;
		return 0;

	case COUNTER_FUNCTION_DECREASE:
		if (!prev_a && a)
			return -1;
		return 0;

	default:
		return 0;
	}
}

static int gpio_qenc_b_delta(struct gpio_qenc_priv *priv, int a, int b,
			     int prev_a, int prev_b)
{
	int state = CREATE_QE_STATE(prev_a, prev_b, a, b);

	switch (priv->function) {
	case COUNTER_FUNCTION_QUADRATURE_X4:
		return gpio_qenc_quad_x4_table[state];

	case COUNTER_FUNCTION_QUADRATURE_X2_B:
		return (a == b) ? 1 : -1;

	case COUNTER_FUNCTION_QUADRATURE_X1_B:
		if (!prev_b && b)
			return a ? 1 : -1;
		return 0;

	default:
		return 0;
	}
}

static irqreturn_t gpio_qenc_a_isr(int irq, void *dev_id)
{
	struct counter_device *counter = dev_id;
	struct gpio_qenc_priv *priv = counter_priv(counter);
	unsigned long flags;
	int a, b, delta;

	spin_lock_irqsave(&priv->lock, flags);

	a = gpiod_get_value(priv->gpio_a);
	b = gpiod_get_value(priv->gpio_b);

	delta = gpio_qenc_a_delta(priv, a, b, priv->prev_a, priv->prev_b);
	gpio_qenc_update_count(priv, delta);

	priv->prev_a = a;
	priv->prev_b = b;

	spin_unlock_irqrestore(&priv->lock, flags);

	counter_push_event(counter, COUNTER_EVENT_CHANGE_OF_STATE, 0);

	return IRQ_HANDLED;
}

static irqreturn_t gpio_qenc_b_isr(int irq, void *dev_id)
{
	struct counter_device *counter = dev_id;
	struct gpio_qenc_priv *priv = counter_priv(counter);
	unsigned long flags;
	int a, b, delta;

	spin_lock_irqsave(&priv->lock, flags);

	a = gpiod_get_value(priv->gpio_a);
	b = gpiod_get_value(priv->gpio_b);

	delta = gpio_qenc_b_delta(priv, a, b, priv->prev_a, priv->prev_b);
	gpio_qenc_update_count(priv, delta);

	priv->prev_a = a;
	priv->prev_b = b;

	spin_unlock_irqrestore(&priv->lock, flags);

	counter_push_event(counter, COUNTER_EVENT_CHANGE_OF_STATE, 0);

	return IRQ_HANDLED;
}

static irqreturn_t gpio_qenc_index_isr(int irq, void *dev_id)
{
	struct counter_device *counter = dev_id;
	struct gpio_qenc_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	if (priv->preset_enabled)
		priv->count = priv->preset;
	spin_unlock_irqrestore(&priv->lock, flags);

	counter_push_event(counter, COUNTER_EVENT_INDEX, 0);

	return IRQ_HANDLED;
}

static int gpio_qenc_count_read(struct counter_device *counter,
				struct counter_count *count, u64 *val)
{
	struct gpio_qenc_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*val = priv->count;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_qenc_count_write(struct counter_device *counter,
				 struct counter_count *count, u64 val)
{
	struct gpio_qenc_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	if (val > priv->ceiling) {
		spin_unlock_irqrestore(&priv->lock, flags);
		return -EINVAL;
	}

	priv->count = val;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static const enum counter_function gpio_qenc_functions[] = {
	COUNTER_FUNCTION_INCREASE,
	COUNTER_FUNCTION_DECREASE,
	COUNTER_FUNCTION_PULSE_DIRECTION,
	COUNTER_FUNCTION_QUADRATURE_X1_A,
	COUNTER_FUNCTION_QUADRATURE_X1_B,
	COUNTER_FUNCTION_QUADRATURE_X2_A,
	COUNTER_FUNCTION_QUADRATURE_X2_B,
	COUNTER_FUNCTION_QUADRATURE_X4,
};

static int gpio_qenc_function_read(struct counter_device *counter,
				   struct counter_count *count,
				   enum counter_function *function)
{
	struct gpio_qenc_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*function = priv->function;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_qenc_function_write(struct counter_device *counter,
				    struct counter_count *count,
				    enum counter_function function)
{
	struct gpio_qenc_priv *priv = counter_priv(counter);
	unsigned long flags;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(gpio_qenc_functions); i++)
		if (gpio_qenc_functions[i] == function)
			break;
	if (i == ARRAY_SIZE(gpio_qenc_functions))
		return -EINVAL;

	spin_lock_irqsave(&priv->lock, flags);
	priv->function = function;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static const enum counter_synapse_action gpio_qenc_synapse_actions[] = {
	COUNTER_SYNAPSE_ACTION_NONE,
	COUNTER_SYNAPSE_ACTION_RISING_EDGE,
	COUNTER_SYNAPSE_ACTION_FALLING_EDGE,
	COUNTER_SYNAPSE_ACTION_BOTH_EDGES,
};

static const enum counter_synapse_action gpio_qenc_index_synapse_actions[] = {
	COUNTER_SYNAPSE_ACTION_NONE,
	COUNTER_SYNAPSE_ACTION_RISING_EDGE,
};

static int gpio_qenc_action_read(struct counter_device *counter,
				 struct counter_count *count,
				 struct counter_synapse *synapse,
				 enum counter_synapse_action *action)
{
	struct gpio_qenc_priv *priv = counter_priv(counter);
	enum gpio_qenc_signal_id signal_id = synapse->signal->id;

	/* Index synapse always observes rising edges, regardless of mode. */
	if (signal_id == GPIO_QENC_SIGNAL_INDEX) {
		*action = COUNTER_SYNAPSE_ACTION_RISING_EDGE;
		return 0;
	}

	*action = COUNTER_SYNAPSE_ACTION_NONE;

	switch (priv->function) {
	case COUNTER_FUNCTION_QUADRATURE_X4:
		*action = COUNTER_SYNAPSE_ACTION_BOTH_EDGES;
		break;

	case COUNTER_FUNCTION_QUADRATURE_X2_A:
		if (signal_id == GPIO_QENC_SIGNAL_A)
			*action = COUNTER_SYNAPSE_ACTION_BOTH_EDGES;
		break;

	case COUNTER_FUNCTION_QUADRATURE_X2_B:
		if (signal_id == GPIO_QENC_SIGNAL_B)
			*action = COUNTER_SYNAPSE_ACTION_BOTH_EDGES;
		break;

	case COUNTER_FUNCTION_QUADRATURE_X1_A:
		if (signal_id == GPIO_QENC_SIGNAL_A) {
			if (priv->direction == COUNTER_COUNT_DIRECTION_FORWARD)
				*action = COUNTER_SYNAPSE_ACTION_RISING_EDGE;
			else
				*action = COUNTER_SYNAPSE_ACTION_FALLING_EDGE;
		}
		break;

	case COUNTER_FUNCTION_QUADRATURE_X1_B:
		if (signal_id == GPIO_QENC_SIGNAL_B) {
			if (priv->direction == COUNTER_COUNT_DIRECTION_FORWARD)
				*action = COUNTER_SYNAPSE_ACTION_RISING_EDGE;
			else
				*action = COUNTER_SYNAPSE_ACTION_FALLING_EDGE;
		}
		break;

	case COUNTER_FUNCTION_PULSE_DIRECTION:
	case COUNTER_FUNCTION_INCREASE:
	case COUNTER_FUNCTION_DECREASE:
		if (signal_id == GPIO_QENC_SIGNAL_A)
			*action = COUNTER_SYNAPSE_ACTION_RISING_EDGE;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int gpio_qenc_signal_read(struct counter_device *counter,
				 struct counter_signal *signal,
				 enum counter_signal_level *level)
{
	struct gpio_qenc_priv *priv = counter_priv(counter);
	struct gpio_desc *gpio;
	int ret;

	switch (signal->id) {
	case GPIO_QENC_SIGNAL_A:
		gpio = priv->gpio_a;
		break;
	case GPIO_QENC_SIGNAL_B:
		gpio = priv->gpio_b;
		break;
	case GPIO_QENC_SIGNAL_INDEX:
		gpio = priv->gpio_index;
		break;
	default:
		return -EINVAL;
	}

	ret = gpiod_get_value(gpio);
	if (ret < 0)
		return ret;

	*level = ret ? COUNTER_SIGNAL_LEVEL_HIGH : COUNTER_SIGNAL_LEVEL_LOW;
	return 0;
}

static int gpio_qenc_watch_validate(struct counter_device *counter,
				    const struct counter_watch *watch)
{
	if (watch->channel != 0)
		return -EINVAL;

	switch (watch->event) {
	case COUNTER_EVENT_CHANGE_OF_STATE:
	case COUNTER_EVENT_INDEX:
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct counter_ops gpio_qenc_ops = {
	.count_read	= gpio_qenc_count_read,
	.count_write	= gpio_qenc_count_write,
	.function_read	= gpio_qenc_function_read,
	.function_write	= gpio_qenc_function_write,
	.action_read	= gpio_qenc_action_read,
	.signal_read	= gpio_qenc_signal_read,
	.watch_validate	= gpio_qenc_watch_validate,
};

static int gpio_qenc_ceiling_read(struct counter_device *counter,
				  struct counter_count *count, u64 *val)
{
	struct gpio_qenc_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*val = priv->ceiling;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_qenc_ceiling_write(struct counter_device *counter,
				   struct counter_count *count, u64 val)
{
	struct gpio_qenc_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	priv->ceiling = val;
	if (priv->count > val)
		priv->count = val;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_qenc_enable_read(struct counter_device *counter,
				 struct counter_count *count, u8 *enable)
{
	struct gpio_qenc_priv *priv = counter_priv(counter);
	unsigned long flags;
	bool irq_enabled;

	spin_lock_irqsave(&priv->lock, flags);
	irq_enabled = !irqd_irq_disabled(irq_get_irq_data(priv->irq_a));
	spin_unlock_irqrestore(&priv->lock, flags);

	*enable = irq_enabled;
	return 0;
}

static int gpio_qenc_enable_write(struct counter_device *counter,
				  struct counter_count *count, u8 enable)
{
	struct gpio_qenc_priv *priv = counter_priv(counter);

	if (enable) {
		enable_irq(priv->irq_a);
		enable_irq(priv->irq_b);
		if (priv->irq_index)
			enable_irq(priv->irq_index);
		return 0;
	}

	disable_irq(priv->irq_a);
	disable_irq(priv->irq_b);
	if (priv->irq_index)
		disable_irq(priv->irq_index);
	return 0;
}

static int gpio_qenc_direction_read(struct counter_device *counter,
				    struct counter_count *count, u32 *direction)
{
	struct gpio_qenc_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*direction = priv->direction;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_qenc_preset_read(struct counter_device *counter,
				 struct counter_count *count, u64 *val)
{
	struct gpio_qenc_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*val = priv->preset;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_qenc_preset_write(struct counter_device *counter,
				  struct counter_count *count, u64 val)
{
	struct gpio_qenc_priv *priv = counter_priv(counter);
	unsigned long flags;

	if (val > priv->ceiling)
		return -EINVAL;

	spin_lock_irqsave(&priv->lock, flags);
	priv->preset = val;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_qenc_preset_enable_read(struct counter_device *counter,
					struct counter_count *count, u8 *val)
{
	struct gpio_qenc_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*val = priv->preset_enabled;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_qenc_preset_enable_write(struct counter_device *counter,
					 struct counter_count *count, u8 val)
{
	struct gpio_qenc_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	priv->preset_enabled = !!val;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static struct counter_comp gpio_qenc_count_ext[] = {
	COUNTER_COMP_CEILING(gpio_qenc_ceiling_read, gpio_qenc_ceiling_write),
	COUNTER_COMP_ENABLE(gpio_qenc_enable_read, gpio_qenc_enable_write),
	COUNTER_COMP_DIRECTION(gpio_qenc_direction_read),
	COUNTER_COMP_PRESET(gpio_qenc_preset_read, gpio_qenc_preset_write),
	COUNTER_COMP_PRESET_ENABLE(gpio_qenc_preset_enable_read,
				   gpio_qenc_preset_enable_write),
};

static int gpio_qenc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct counter_device *counter;
	struct gpio_qenc_priv *priv;
	bool has_index;
	int num_signals;
	int num_synapses;
	int ret;

	counter = devm_counter_alloc(dev, sizeof(*priv));
	if (!counter)
		return -ENOMEM;

	priv = counter_priv(counter);
	spin_lock_init(&priv->lock);

	priv->gpio_a = devm_gpiod_get(dev, "encoder-a", GPIOD_IN);
	if (IS_ERR(priv->gpio_a))
		return dev_err_probe(dev, PTR_ERR(priv->gpio_a),
				     "failed to get encoder-a GPIO\n");

	priv->gpio_b = devm_gpiod_get(dev, "encoder-b", GPIOD_IN);
	if (IS_ERR(priv->gpio_b))
		return dev_err_probe(dev, PTR_ERR(priv->gpio_b),
				     "failed to get encoder-b GPIO\n");

	priv->gpio_index = devm_gpiod_get_optional(dev, "encoder-index",
						   GPIOD_IN);
	if (IS_ERR(priv->gpio_index))
		return dev_err_probe(dev, PTR_ERR(priv->gpio_index),
				     "failed to get encoder-index GPIO\n");

	has_index = !!priv->gpio_index;

	priv->irq_a = gpiod_to_irq(priv->gpio_a);
	if (priv->irq_a < 0)
		return dev_err_probe(dev, priv->irq_a,
				     "failed to get IRQ for encoder-a\n");

	priv->irq_b = gpiod_to_irq(priv->gpio_b);
	if (priv->irq_b < 0)
		return dev_err_probe(dev, priv->irq_b,
				     "failed to get IRQ for encoder-b\n");

	if (has_index) {
		priv->irq_index = gpiod_to_irq(priv->gpio_index);
		if (priv->irq_index < 0)
			return dev_err_probe(dev, priv->irq_index,
					     "failed to get IRQ for encoder-index\n");
	}

	priv->prev_a = gpiod_get_value(priv->gpio_a);
	priv->prev_b = gpiod_get_value(priv->gpio_b);

	priv->function = COUNTER_FUNCTION_QUADRATURE_X4;
	priv->direction = COUNTER_COUNT_DIRECTION_FORWARD;
	priv->ceiling = U64_MAX;

	num_signals = has_index ? 3 : 2;
	num_synapses = num_signals;

	priv->signals[GPIO_QENC_SIGNAL_A].id = GPIO_QENC_SIGNAL_A;
	priv->signals[GPIO_QENC_SIGNAL_A].name = "Signal A";

	priv->signals[GPIO_QENC_SIGNAL_B].id = GPIO_QENC_SIGNAL_B;
	priv->signals[GPIO_QENC_SIGNAL_B].name = "Signal B";

	priv->synapses[0].actions_list = gpio_qenc_synapse_actions;
	priv->synapses[0].num_actions = ARRAY_SIZE(gpio_qenc_synapse_actions);
	priv->synapses[0].signal = &priv->signals[GPIO_QENC_SIGNAL_A];

	priv->synapses[1].actions_list = gpio_qenc_synapse_actions;
	priv->synapses[1].num_actions = ARRAY_SIZE(gpio_qenc_synapse_actions);
	priv->synapses[1].signal = &priv->signals[GPIO_QENC_SIGNAL_B];

	if (has_index) {
		priv->signals[GPIO_QENC_SIGNAL_INDEX].id =
			GPIO_QENC_SIGNAL_INDEX;
		priv->signals[GPIO_QENC_SIGNAL_INDEX].name = "Index";

		priv->synapses[2].actions_list = gpio_qenc_index_synapse_actions;
		priv->synapses[2].num_actions =
			ARRAY_SIZE(gpio_qenc_index_synapse_actions);
		priv->synapses[2].signal =
			&priv->signals[GPIO_QENC_SIGNAL_INDEX];
	}

	priv->cnts[0].id = 0;
	priv->cnts[0].name = "Count";
	priv->cnts[0].functions_list = gpio_qenc_functions;
	priv->cnts[0].num_functions = ARRAY_SIZE(gpio_qenc_functions);
	priv->cnts[0].synapses = priv->synapses;
	priv->cnts[0].num_synapses = num_synapses;
	priv->cnts[0].ext = gpio_qenc_count_ext;
	priv->cnts[0].num_ext = ARRAY_SIZE(gpio_qenc_count_ext);

	counter->name = dev_name(dev);
	counter->parent = dev;
	counter->ops = &gpio_qenc_ops;
	counter->signals = priv->signals;
	counter->num_signals = num_signals;
	counter->counts = priv->cnts;
	counter->num_counts = ARRAY_SIZE(priv->cnts);

	irq_set_status_flags(priv->irq_a, IRQ_NOAUTOEN);
	ret = devm_request_irq(dev, priv->irq_a, gpio_qenc_a_isr,
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			       "gpio-qenc-a", counter);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to request IRQ for encoder-a\n");

	irq_set_status_flags(priv->irq_b, IRQ_NOAUTOEN);
	ret = devm_request_irq(dev, priv->irq_b, gpio_qenc_b_isr,
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			       "gpio-qenc-b", counter);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to request IRQ for encoder-b\n");

	if (has_index) {
		irq_set_status_flags(priv->irq_index, IRQ_NOAUTOEN);
		ret = devm_request_irq(dev, priv->irq_index,
				       gpio_qenc_index_isr,
				       IRQF_TRIGGER_RISING,
				       "gpio-qenc-index", counter);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to request IRQ for encoder-index\n");
	}

	ret = devm_counter_add(dev, counter);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to add counter\n");

	dev_info(dev, "GPIO quadrature encoder registered (signals: A, B%s)\n",
		 has_index ? ", Index" : "");

	return 0;
}

static const struct of_device_id gpio_qenc_of_match[] = {
	{ .compatible = "gpio-quadrature-encoder" },
	{}
};
MODULE_DEVICE_TABLE(of, gpio_qenc_of_match);

static struct platform_driver gpio_qenc_driver = {
	.probe = gpio_qenc_probe,
	.driver = {
		.name = "gpio-quadrature-encoder",
		.of_match_table = gpio_qenc_of_match,
	},
};
module_platform_driver(gpio_qenc_driver);

MODULE_ALIAS("platform:gpio-quadrature-encoder");
MODULE_AUTHOR("Wadim Mueller <wafgo01@gmail.com>");
MODULE_DESCRIPTION("GPIO-based quadrature encoder counter driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("COUNTER");
