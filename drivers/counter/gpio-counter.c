// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO-based Counter Driver
 *
 * Decodes A/B (and optional Index) signals from GPIO lines in software.
 * Supports quadrature X1/X2/X4, pulse-direction, and pure
 * increase/decrease modes.
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

enum gpio_counter_signal_id {
	GPIO_COUNTER_SIGNAL_A = 0,
	GPIO_COUNTER_SIGNAL_B,
	GPIO_COUNTER_SIGNAL_INDEX,
};

struct gpio_counter_priv {
	struct gpio_desc *gpio_a;
	struct gpio_desc *gpio_b;
	struct gpio_desc *gpio_index;

	int irq_a;
	int irq_b;
	int irq_index;

	spinlock_t lock; /* protects count, ceiling, preset, function, direction, enabled */

	u64 count;
	u64 ceiling;
	u64 preset;
	bool preset_enabled;
	bool enabled;
	enum counter_count_direction direction;
	enum counter_function function;

	int prev_a;
	int prev_b;

	struct counter_count cnts[1];
	struct counter_signal signals[3];
	struct counter_synapse synapses[3];
};

/* X4 decode via 2-bit Gray-code parity. */
#define GPIO_COUNTER_STATE_CHANGED(pa, pb, ca, cb) ((pa) ^ (pb) ^ (ca) ^ (cb))
#define GPIO_COUNTER_GET_DIRECTION(pb, ca)                 \
	(((pb) ^ (ca)) ? COUNTER_COUNT_DIRECTION_FORWARD : \
			 COUNTER_COUNT_DIRECTION_BACKWARD)

static void gpio_counter_update(struct gpio_counter_priv *priv, int delta)
{
	if (delta > 0) {
		priv->direction = COUNTER_COUNT_DIRECTION_FORWARD;
		if (priv->count >= priv->ceiling)
			return;
		priv->count++;
	} else if (delta < 0) {
		priv->direction = COUNTER_COUNT_DIRECTION_BACKWARD;
		if (priv->count == 0)
			return;
		priv->count--;
	}
}

static int gpio_counter_a_delta(struct gpio_counter_priv *priv, int a, int b,
				int prev_a, int prev_b)
{
	enum counter_count_direction dir;

	switch (priv->function) {
	case COUNTER_FUNCTION_QUADRATURE_X4:
		if (!GPIO_COUNTER_STATE_CHANGED(prev_a, prev_b, a, b))
			return 0;
		dir = GPIO_COUNTER_GET_DIRECTION(prev_b, a);
		return (dir == COUNTER_COUNT_DIRECTION_FORWARD) ? 1 : -1;

	case COUNTER_FUNCTION_QUADRATURE_X2_A:
		return (a == b) ? -1 : 1;

	case COUNTER_FUNCTION_QUADRATURE_X1_A:
		if (a && priv->direction == COUNTER_COUNT_DIRECTION_FORWARD)
			return 1;
		if (!a && priv->direction == COUNTER_COUNT_DIRECTION_BACKWARD)
			return -1;
		return 0;

	case COUNTER_FUNCTION_PULSE_DIRECTION:
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

static int gpio_counter_b_delta(struct gpio_counter_priv *priv, int a, int b,
				int prev_a, int prev_b)
{
	enum counter_count_direction dir;

	switch (priv->function) {
	case COUNTER_FUNCTION_QUADRATURE_X4:
		if (!GPIO_COUNTER_STATE_CHANGED(prev_a, prev_b, a, b))
			return 0;
		dir = GPIO_COUNTER_GET_DIRECTION(prev_b, a);
		return (dir == COUNTER_COUNT_DIRECTION_FORWARD) ? 1 : -1;

	case COUNTER_FUNCTION_QUADRATURE_X2_B:
		return (a == b) ? 1 : -1;

	case COUNTER_FUNCTION_QUADRATURE_X1_B:
		if (b && priv->direction == COUNTER_COUNT_DIRECTION_FORWARD)
			return 1;
		if (!b && priv->direction == COUNTER_COUNT_DIRECTION_BACKWARD)
			return -1;
		return 0;

	default:
		return 0;
	}
}

static irqreturn_t gpio_counter_a_isr(int irq, void *dev_id)
{
	struct counter_device *counter = dev_id;
	struct gpio_counter_priv *priv = counter_priv(counter);
	unsigned long flags;
	int a, b, delta;

	/* !! normalises away negative gpiod_get_value() errors. */
	a = !!gpiod_get_value(priv->gpio_a);
	b = !!gpiod_get_value(priv->gpio_b);

	spin_lock_irqsave(&priv->lock, flags);

	delta = gpio_counter_a_delta(priv, a, b, priv->prev_a, priv->prev_b);
	gpio_counter_update(priv, delta);

	priv->prev_a = a;
	priv->prev_b = b;

	spin_unlock_irqrestore(&priv->lock, flags);

	counter_push_event(counter, COUNTER_EVENT_CHANGE_OF_STATE, 0);

	return IRQ_HANDLED;
}

static irqreturn_t gpio_counter_b_isr(int irq, void *dev_id)
{
	struct counter_device *counter = dev_id;
	struct gpio_counter_priv *priv = counter_priv(counter);
	unsigned long flags;
	int a, b, delta;

	a = !!gpiod_get_value(priv->gpio_a);
	b = !!gpiod_get_value(priv->gpio_b);

	spin_lock_irqsave(&priv->lock, flags);

	delta = gpio_counter_b_delta(priv, a, b, priv->prev_a, priv->prev_b);
	gpio_counter_update(priv, delta);

	priv->prev_a = a;
	priv->prev_b = b;

	spin_unlock_irqrestore(&priv->lock, flags);

	counter_push_event(counter, COUNTER_EVENT_CHANGE_OF_STATE, 0);

	return IRQ_HANDLED;
}

static irqreturn_t gpio_counter_index_isr(int irq, void *dev_id)
{
	struct counter_device *counter = dev_id;
	struct gpio_counter_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	if (priv->preset_enabled) {
		priv->count = priv->preset;
		if (priv->count > priv->ceiling)
			priv->count = priv->ceiling;
	}

	spin_unlock_irqrestore(&priv->lock, flags);

	counter_push_event(counter, COUNTER_EVENT_INDEX, 0);

	return IRQ_HANDLED;
}

static int gpio_counter_count_read(struct counter_device *counter,
				   struct counter_count *count, u64 *val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*val = priv->count;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_count_write(struct counter_device *counter,
				    struct counter_count *count, u64 val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
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

static const enum counter_function gpio_counter_functions[] = {
	COUNTER_FUNCTION_INCREASE,	  COUNTER_FUNCTION_DECREASE,
	COUNTER_FUNCTION_PULSE_DIRECTION, COUNTER_FUNCTION_QUADRATURE_X1_A,
	COUNTER_FUNCTION_QUADRATURE_X1_B, COUNTER_FUNCTION_QUADRATURE_X2_A,
	COUNTER_FUNCTION_QUADRATURE_X2_B, COUNTER_FUNCTION_QUADRATURE_X4,
};

static int gpio_counter_function_read(struct counter_device *counter,
				      struct counter_count *count,
				      enum counter_function *function)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*function = priv->function;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_function_write(struct counter_device *counter,
				       struct counter_count *count,
				       enum counter_function function)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	priv->function = function;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static const enum counter_synapse_action gpio_counter_synapse_actions[] = {
	COUNTER_SYNAPSE_ACTION_NONE,
	COUNTER_SYNAPSE_ACTION_RISING_EDGE,
	COUNTER_SYNAPSE_ACTION_FALLING_EDGE,
	COUNTER_SYNAPSE_ACTION_BOTH_EDGES,
};

static const enum counter_synapse_action gpio_counter_index_synapse_actions[] = {
	COUNTER_SYNAPSE_ACTION_NONE,
	COUNTER_SYNAPSE_ACTION_RISING_EDGE,
};

static int gpio_counter_action_read(struct counter_device *counter,
				    struct counter_count *count,
				    struct counter_synapse *synapse,
				    enum counter_synapse_action *action)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	enum gpio_counter_signal_id signal_id = synapse->signal->id;
	enum counter_function function;
	enum counter_count_direction direction;
	unsigned long flags;

	if (signal_id == GPIO_COUNTER_SIGNAL_INDEX) {
		*action = COUNTER_SYNAPSE_ACTION_RISING_EDGE;
		return 0;
	}

	spin_lock_irqsave(&priv->lock, flags);
	function = priv->function;
	direction = priv->direction;
	spin_unlock_irqrestore(&priv->lock, flags);

	*action = COUNTER_SYNAPSE_ACTION_NONE;

	switch (function) {
	case COUNTER_FUNCTION_QUADRATURE_X4:
		*action = COUNTER_SYNAPSE_ACTION_BOTH_EDGES;
		return 0;

	case COUNTER_FUNCTION_QUADRATURE_X2_A:
		if (signal_id == GPIO_COUNTER_SIGNAL_A)
			*action = COUNTER_SYNAPSE_ACTION_BOTH_EDGES;
		return 0;

	case COUNTER_FUNCTION_QUADRATURE_X2_B:
		if (signal_id == GPIO_COUNTER_SIGNAL_B)
			*action = COUNTER_SYNAPSE_ACTION_BOTH_EDGES;
		return 0;

	case COUNTER_FUNCTION_QUADRATURE_X1_A:
		if (signal_id == GPIO_COUNTER_SIGNAL_A) {
			if (direction == COUNTER_COUNT_DIRECTION_FORWARD)
				*action = COUNTER_SYNAPSE_ACTION_RISING_EDGE;
			else
				*action = COUNTER_SYNAPSE_ACTION_FALLING_EDGE;
		}
		return 0;

	case COUNTER_FUNCTION_QUADRATURE_X1_B:
		if (signal_id == GPIO_COUNTER_SIGNAL_B) {
			if (direction == COUNTER_COUNT_DIRECTION_FORWARD)
				*action = COUNTER_SYNAPSE_ACTION_RISING_EDGE;
			else
				*action = COUNTER_SYNAPSE_ACTION_FALLING_EDGE;
		}
		return 0;

	case COUNTER_FUNCTION_PULSE_DIRECTION:
	case COUNTER_FUNCTION_INCREASE:
	case COUNTER_FUNCTION_DECREASE:
		if (signal_id == GPIO_COUNTER_SIGNAL_A)
			*action = COUNTER_SYNAPSE_ACTION_RISING_EDGE;
		return 0;

	default:
		return -EINVAL;
	}
}

static int gpio_counter_signal_read(struct counter_device *counter,
				    struct counter_signal *signal,
				    enum counter_signal_level *level)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_desc *gpio;
	int ret;

	switch (signal->id) {
	case GPIO_COUNTER_SIGNAL_A:
		gpio = priv->gpio_a;
		break;
	case GPIO_COUNTER_SIGNAL_B:
		gpio = priv->gpio_b;
		break;
	case GPIO_COUNTER_SIGNAL_INDEX:
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

static int gpio_counter_watch_validate(struct counter_device *counter,
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

static const struct counter_ops gpio_counter_ops = {
	.count_read = gpio_counter_count_read,
	.count_write = gpio_counter_count_write,
	.function_read = gpio_counter_function_read,
	.function_write = gpio_counter_function_write,
	.action_read = gpio_counter_action_read,
	.signal_read = gpio_counter_signal_read,
	.watch_validate = gpio_counter_watch_validate,
};

static int gpio_counter_ceiling_read(struct counter_device *counter,
				     struct counter_count *count, u64 *val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*val = priv->ceiling;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_ceiling_write(struct counter_device *counter,
				      struct counter_count *count, u64 val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	unsigned long flags;

	/* Leave count untouched on shrink; matches intel-qep / ti-eqep / stm32-timer-cnt. */
	spin_lock_irqsave(&priv->lock, flags);
	priv->ceiling = val;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_enable_read(struct counter_device *counter,
				    struct counter_count *count, u8 *enable)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*enable = priv->enabled;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_enable_write(struct counter_device *counter,
				     struct counter_count *count, u8 enable)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	unsigned long flags;
	bool want = enable;
	bool changed;

	spin_lock_irqsave(&priv->lock, flags);
	changed = priv->enabled != want;
	if (changed)
		priv->enabled = want;
	spin_unlock_irqrestore(&priv->lock, flags);

	if (!changed)
		return 0;

	if (want) {
		enable_irq(priv->irq_a);
		enable_irq(priv->irq_b);
		if (priv->irq_index)
			enable_irq(priv->irq_index);
	} else {
		disable_irq(priv->irq_a);
		disable_irq(priv->irq_b);
		if (priv->irq_index)
			disable_irq(priv->irq_index);
	}

	return 0;
}

static int gpio_counter_direction_read(struct counter_device *counter,
				       struct counter_count *count,
				       u32 *direction)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*direction = priv->direction;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_preset_read(struct counter_device *counter,
				    struct counter_count *count, u64 *val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*val = priv->preset;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_preset_write(struct counter_device *counter,
				     struct counter_count *count, u64 val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&priv->lock, flags);
	if (val > priv->ceiling) {
		ret = -EINVAL;
		goto out;
	}
	priv->preset = val;
out:
	spin_unlock_irqrestore(&priv->lock, flags);

	return ret;
}

static int gpio_counter_preset_enable_read(struct counter_device *counter,
					   struct counter_count *count, u8 *val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*val = priv->preset_enabled;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_preset_enable_write(struct counter_device *counter,
					    struct counter_count *count, u8 val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	priv->preset_enabled = val;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static struct counter_comp gpio_counter_count_ext[] = {
	COUNTER_COMP_CEILING(gpio_counter_ceiling_read,
			     gpio_counter_ceiling_write),
	COUNTER_COMP_ENABLE(gpio_counter_enable_read,
			    gpio_counter_enable_write),
	COUNTER_COMP_DIRECTION(gpio_counter_direction_read),
	COUNTER_COMP_PRESET(gpio_counter_preset_read,
			    gpio_counter_preset_write),
	COUNTER_COMP_PRESET_ENABLE(gpio_counter_preset_enable_read,
				   gpio_counter_preset_enable_write),
};

static int gpio_counter_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct counter_device *counter;
	struct gpio_counter_priv *priv;
	bool has_index;
	int num_signals;
	int num_synapses;
	int ret;

	counter = devm_counter_alloc(dev, sizeof(*priv));
	if (!counter)
		return -ENOMEM;

	priv = counter_priv(counter);
	spin_lock_init(&priv->lock);

	priv->gpio_a = devm_gpiod_get(dev, "signal-a", GPIOD_IN);
	if (IS_ERR(priv->gpio_a))
		return dev_err_probe(dev, PTR_ERR(priv->gpio_a),
				     "failed to get signal-a GPIO\n");

	priv->gpio_b = devm_gpiod_get(dev, "signal-b", GPIOD_IN);
	if (IS_ERR(priv->gpio_b))
		return dev_err_probe(dev, PTR_ERR(priv->gpio_b),
				     "failed to get signal-b GPIO\n");

	priv->gpio_index = devm_gpiod_get_optional(dev, "index", GPIOD_IN);
	if (IS_ERR(priv->gpio_index))
		return dev_err_probe(dev, PTR_ERR(priv->gpio_index),
				     "failed to get index GPIO\n");

	has_index = !!priv->gpio_index;

	if (gpiod_cansleep(priv->gpio_a) || gpiod_cansleep(priv->gpio_b) ||
	    (has_index && gpiod_cansleep(priv->gpio_index)))
		return dev_err_probe(dev, -EINVAL,
				     "GPIO controller may sleep; not supported in IRQ context\n");

	priv->irq_a = gpiod_to_irq(priv->gpio_a);
	if (priv->irq_a < 0)
		return dev_err_probe(dev, priv->irq_a,
				     "failed to get IRQ for signal-a\n");

	priv->irq_b = gpiod_to_irq(priv->gpio_b);
	if (priv->irq_b < 0)
		return dev_err_probe(dev, priv->irq_b,
				     "failed to get IRQ for signal-b\n");

	if (has_index) {
		priv->irq_index = gpiod_to_irq(priv->gpio_index);
		if (priv->irq_index < 0)
			return dev_err_probe(dev, priv->irq_index,
					     "failed to get IRQ for index\n");
	}

	priv->prev_a = !!gpiod_get_value(priv->gpio_a);
	priv->prev_b = !!gpiod_get_value(priv->gpio_b);

	priv->function = COUNTER_FUNCTION_QUADRATURE_X4;
	priv->direction = COUNTER_COUNT_DIRECTION_FORWARD;
	priv->ceiling = U64_MAX;
	priv->enabled = false;

	num_signals = has_index ? 3 : 2;
	num_synapses = num_signals;

	priv->signals[GPIO_COUNTER_SIGNAL_A].id = GPIO_COUNTER_SIGNAL_A;
	priv->signals[GPIO_COUNTER_SIGNAL_A].name = "Signal A";

	priv->signals[GPIO_COUNTER_SIGNAL_B].id = GPIO_COUNTER_SIGNAL_B;
	priv->signals[GPIO_COUNTER_SIGNAL_B].name = "Signal B";

	priv->synapses[0].actions_list = gpio_counter_synapse_actions;
	priv->synapses[0].num_actions =
		ARRAY_SIZE(gpio_counter_synapse_actions);
	priv->synapses[0].signal = &priv->signals[GPIO_COUNTER_SIGNAL_A];

	priv->synapses[1].actions_list = gpio_counter_synapse_actions;
	priv->synapses[1].num_actions =
		ARRAY_SIZE(gpio_counter_synapse_actions);
	priv->synapses[1].signal = &priv->signals[GPIO_COUNTER_SIGNAL_B];

	if (has_index) {
		priv->signals[GPIO_COUNTER_SIGNAL_INDEX].id =
			GPIO_COUNTER_SIGNAL_INDEX;
		priv->signals[GPIO_COUNTER_SIGNAL_INDEX].name = "Index";

		priv->synapses[2].actions_list =
			gpio_counter_index_synapse_actions;
		priv->synapses[2].num_actions =
			ARRAY_SIZE(gpio_counter_index_synapse_actions);
		priv->synapses[2].signal =
			&priv->signals[GPIO_COUNTER_SIGNAL_INDEX];
	}

	priv->cnts[0].id = 0;
	priv->cnts[0].name = "Count";
	priv->cnts[0].functions_list = gpio_counter_functions;
	priv->cnts[0].num_functions = ARRAY_SIZE(gpio_counter_functions);
	priv->cnts[0].synapses = priv->synapses;
	priv->cnts[0].num_synapses = num_synapses;
	priv->cnts[0].ext = gpio_counter_count_ext;
	priv->cnts[0].num_ext = ARRAY_SIZE(gpio_counter_count_ext);

	counter->name = dev_name(dev);
	counter->parent = dev;
	counter->ops = &gpio_counter_ops;
	counter->signals = priv->signals;
	counter->num_signals = num_signals;
	counter->counts = priv->cnts;
	counter->num_counts = ARRAY_SIZE(priv->cnts);

	ret = devm_request_irq(dev, priv->irq_a, gpio_counter_a_isr,
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				       IRQF_NO_AUTOEN,
			       "gpio-counter-a", counter);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to request IRQ for signal-a\n");

	ret = devm_request_irq(dev, priv->irq_b, gpio_counter_b_isr,
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				       IRQF_NO_AUTOEN,
			       "gpio-counter-b", counter);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to request IRQ for signal-b\n");

	if (has_index) {
		ret = devm_request_irq(dev, priv->irq_index,
				       gpio_counter_index_isr,
				       IRQF_TRIGGER_RISING | IRQF_NO_AUTOEN,
				       "gpio-counter-index", counter);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to request IRQ for index\n");
	}

	ret = devm_counter_add(dev, counter);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to add counter\n");

	dev_info(dev, "GPIO counter registered (signals: A, B%s)\n",
		 has_index ? ", Index" : "");

	return 0;
}

static const struct of_device_id gpio_counter_of_match[] = {
	{ .compatible = "gpio-counter" },
	{}
};
MODULE_DEVICE_TABLE(of, gpio_counter_of_match);

static struct platform_driver gpio_counter_driver = {
	.probe = gpio_counter_probe,
	.driver = {
		.name = "gpio-counter",
		.of_match_table = gpio_counter_of_match,
	},
};
module_platform_driver(gpio_counter_driver);

MODULE_ALIAS("platform:gpio-counter");
MODULE_AUTHOR("Wadim Mueller <wafgo01@gmail.com>");
MODULE_DESCRIPTION("GPIO-based counter driver (quadrature, pulse-direction, increase/decrease)");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("COUNTER");
