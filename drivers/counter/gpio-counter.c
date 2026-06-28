// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO-based Counter Driver
 *
 * Software A/B (+ optional Z) edge decoding exposed as two Counts:
 *   Count 1: A + B (+ Index 1). Quadrature, pulse-direction, inc/dec.
 *   Count 2: B (+ Index 2). Inc / dec.
 *
 * Copyright (C) 2026 CMBlu Energy AG
 * Author: Wadim Mueller <wafgo01@gmail.com>
 */

#include <linux/cleanup.h>
#include <linux/counter.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/types.h>

enum gpio_counter_signal_id {
	GPIO_COUNTER_SIGNAL_A = 0,
	GPIO_COUNTER_SIGNAL_B,
	GPIO_COUNTER_SIGNAL_INDEX1,
	GPIO_COUNTER_SIGNAL_INDEX2,
	GPIO_COUNTER_NUM_SIGNALS_MAX,
};

enum gpio_counter_count_id {
	GPIO_COUNTER_COUNT_1 = 0,
	GPIO_COUNTER_COUNT_2,
	GPIO_COUNTER_NUM_COUNTS,
};

#define GPIO_COUNTER_COUNT1_NUM_SYNAPSES	3	/* A, B, Index 1 */
#define GPIO_COUNTER_COUNT2_NUM_SYNAPSES	2	/* B, Index 2 */
#define GPIO_COUNTER_MAX_INDEX			2

/* per-count state */
struct gpio_counter_count_priv {
	u64				value;
	u64				ceiling;
	u64				preset;
	bool				preset_enabled;
	bool				enabled;
	enum counter_function		function;
	enum counter_count_direction	direction;
};

struct gpio_counter_priv {
	struct gpio_desc *gpio_a;
	struct gpio_desc *gpio_b;
	struct gpio_desc *gpio_idx[GPIO_COUNTER_MAX_INDEX];

	int irq_a;
	int irq_b;
	int irq_idx[GPIO_COUNTER_MAX_INDEX];

	int n_idx;

	/* protects count state, prev_a, prev_b */
	spinlock_t lock;

	/* serialises enable_write() callers and b_irq_users */
	struct mutex enable_lock;

	/* irq_b is shared by Count 1 and Count 2 (under enable_lock) */
	int b_irq_users;

	int prev_a;
	int prev_b;

	struct gpio_counter_count_priv count_priv[GPIO_COUNTER_NUM_COUNTS];

	struct counter_signal  signals[GPIO_COUNTER_NUM_SIGNALS_MAX];
	struct counter_synapse synapses_count1[GPIO_COUNTER_COUNT1_NUM_SYNAPSES];
	struct counter_synapse synapses_count2[GPIO_COUNTER_COUNT2_NUM_SYNAPSES];
	struct counter_count   counts[GPIO_COUNTER_NUM_COUNTS];
};

/* Gray-code: valid step <=> exactly one of A/B toggled. */
#define GPIO_COUNTER_STATE_CHANGED(pa, pb, ca, cb)	\
	((pa) ^ (pb) ^ (ca) ^ (cb))
#define GPIO_COUNTER_GET_DIRECTION(pb, ca)			\
	(((pb) ^ (ca)) ? COUNTER_COUNT_DIRECTION_FORWARD :	\
			 COUNTER_COUNT_DIRECTION_BACKWARD)

/* priv->lock held. */
static void gpio_counter_value_step(struct gpio_counter_count_priv *cp)
{
	if (cp->direction == COUNTER_COUNT_DIRECTION_FORWARD) {
		if (cp->value < cp->ceiling)
			cp->value++;
	} else {
		if (cp->value > 0)
			cp->value--;
	}
}

/* priv->lock held. */
static void gpio_counter_quadrature_x1_step(struct gpio_counter_count_priv *cp,
					    int level)
{
	if (level && cp->direction == COUNTER_COUNT_DIRECTION_FORWARD)
		gpio_counter_value_step(cp);
	else if (!level && cp->direction == COUNTER_COUNT_DIRECTION_BACKWARD)
		gpio_counter_value_step(cp);
}

static irqreturn_t gpio_counter_a_isr(int irq, void *dev_id)
{
	struct counter_device *counter = dev_id;
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp =
		&priv->count_priv[GPIO_COUNTER_COUNT_1];
	unsigned long flags;
	bool enabled;
	int a, b;

	/* !! drops negative gpiod_get_value() errors. */
	a = !!gpiod_get_value(priv->gpio_a);
	b = !!gpiod_get_value(priv->gpio_b);

	spin_lock_irqsave(&priv->lock, flags);

	enabled = cp->enabled;
	if (!enabled)
		goto out;

	switch (cp->function) {
	case COUNTER_FUNCTION_QUADRATURE_X4:
	case COUNTER_FUNCTION_QUADRATURE_X2_A:
	case COUNTER_FUNCTION_QUADRATURE_X1_A:
		if (!GPIO_COUNTER_STATE_CHANGED(priv->prev_a, priv->prev_b,
						a, b))
			break;
		cp->direction = GPIO_COUNTER_GET_DIRECTION(priv->prev_b, a);
		if (cp->function == COUNTER_FUNCTION_QUADRATURE_X1_A)
			gpio_counter_quadrature_x1_step(cp, a);
		else
			gpio_counter_value_step(cp);
		break;

	case COUNTER_FUNCTION_PULSE_DIRECTION:
		/* direction set in the B-ISR */
		if (a)
			gpio_counter_value_step(cp);
		break;

	case COUNTER_FUNCTION_INCREASE:
	case COUNTER_FUNCTION_DECREASE:
		gpio_counter_value_step(cp);
		break;

	default:
		break;
	}

out:
	priv->prev_a = a;
	priv->prev_b = b;

	spin_unlock_irqrestore(&priv->lock, flags);

	if (enabled)
		counter_push_event(counter, COUNTER_EVENT_CHANGE_OF_STATE,
				   GPIO_COUNTER_COUNT_1);

	return IRQ_HANDLED;
}

static irqreturn_t gpio_counter_b_isr(int irq, void *dev_id)
{
	struct counter_device *counter = dev_id;
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *c1 =
		&priv->count_priv[GPIO_COUNTER_COUNT_1];
	struct gpio_counter_count_priv *c2 =
		&priv->count_priv[GPIO_COUNTER_COUNT_2];
	bool c1_enabled, c2_enabled;
	unsigned long flags;
	int a, b;

	a = !!gpiod_get_value(priv->gpio_a);
	b = !!gpiod_get_value(priv->gpio_b);

	spin_lock_irqsave(&priv->lock, flags);

	c1_enabled = c1->enabled;
	c2_enabled = c2->enabled;

	/* Count 1: A+B decoding on this B edge */
	if (c1_enabled) {
		switch (c1->function) {
		case COUNTER_FUNCTION_QUADRATURE_X4:
		case COUNTER_FUNCTION_QUADRATURE_X2_B:
		case COUNTER_FUNCTION_QUADRATURE_X1_B:
			if (!GPIO_COUNTER_STATE_CHANGED(priv->prev_a,
							priv->prev_b, a, b))
				break;
			c1->direction =
				GPIO_COUNTER_GET_DIRECTION(priv->prev_b, a);
			if (c1->function == COUNTER_FUNCTION_QUADRATURE_X1_B)
				gpio_counter_quadrature_x1_step(c1, b);
			else
				gpio_counter_value_step(c1);
			break;

		case COUNTER_FUNCTION_PULSE_DIRECTION:
			/* B is the direction line */
			c1->direction = b ? COUNTER_COUNT_DIRECTION_BACKWARD
					  : COUNTER_COUNT_DIRECTION_FORWARD;
			break;

		default:
			break;
		}
	}

	/* Count 2: B-only */
	if (c2_enabled)
		gpio_counter_value_step(c2);

	priv->prev_a = a;
	priv->prev_b = b;

	spin_unlock_irqrestore(&priv->lock, flags);

	if (c1_enabled)
		counter_push_event(counter, COUNTER_EVENT_CHANGE_OF_STATE,
				   GPIO_COUNTER_COUNT_1);
	if (c2_enabled)
		counter_push_event(counter, COUNTER_EVENT_CHANGE_OF_STATE,
				   GPIO_COUNTER_COUNT_2);

	return IRQ_HANDLED;
}

static void gpio_counter_index_pulse(struct counter_device *counter,
				     enum gpio_counter_count_id count_id)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp = &priv->count_priv[count_id];
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	if (cp->preset_enabled) {
		cp->value = cp->preset;
		if (cp->value > cp->ceiling)
			cp->value = cp->ceiling;
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	counter_push_event(counter, COUNTER_EVENT_INDEX, count_id);
}

static irqreturn_t gpio_counter_index1_isr(int irq, void *dev_id)
{
	gpio_counter_index_pulse(dev_id, GPIO_COUNTER_COUNT_1);
	return IRQ_HANDLED;
}

static irqreturn_t gpio_counter_index2_isr(int irq, void *dev_id)
{
	gpio_counter_index_pulse(dev_id, GPIO_COUNTER_COUNT_2);
	return IRQ_HANDLED;
}

static int gpio_counter_count_read(struct counter_device *counter,
				   struct counter_count *count, u64 *val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp = &priv->count_priv[count->id];
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*val = cp->value;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_count_write(struct counter_device *counter,
				    struct counter_count *count, u64 val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp = &priv->count_priv[count->id];
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	if (val > cp->ceiling) {
		spin_unlock_irqrestore(&priv->lock, flags);
		return -EINVAL;
	}
	cp->value = val;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static const enum counter_function gpio_counter_count1_functions[] = {
	COUNTER_FUNCTION_INCREASE,
	COUNTER_FUNCTION_DECREASE,
	COUNTER_FUNCTION_PULSE_DIRECTION,
	COUNTER_FUNCTION_QUADRATURE_X1_A,
	COUNTER_FUNCTION_QUADRATURE_X1_B,
	COUNTER_FUNCTION_QUADRATURE_X2_A,
	COUNTER_FUNCTION_QUADRATURE_X2_B,
	COUNTER_FUNCTION_QUADRATURE_X4,
};

static const enum counter_function gpio_counter_count2_functions[] = {
	COUNTER_FUNCTION_INCREASE,
	COUNTER_FUNCTION_DECREASE,
};

static int gpio_counter_function_read(struct counter_device *counter,
				      struct counter_count *count,
				      enum counter_function *function)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp = &priv->count_priv[count->id];
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*function = cp->function;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_function_write(struct counter_device *counter,
				       struct counter_count *count,
				       enum counter_function function)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp = &priv->count_priv[count->id];
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	cp->function = function;

	switch (function) {
	case COUNTER_FUNCTION_INCREASE:
		cp->direction = COUNTER_COUNT_DIRECTION_FORWARD;
		break;
	case COUNTER_FUNCTION_DECREASE:
		cp->direction = COUNTER_COUNT_DIRECTION_BACKWARD;
		break;
	case COUNTER_FUNCTION_PULSE_DIRECTION:
		cp->direction = !!gpiod_get_value(priv->gpio_b)
					? COUNTER_COUNT_DIRECTION_BACKWARD
					: COUNTER_COUNT_DIRECTION_FORWARD;
		break;
	default:
		/* quadrature: direction is set in the ISR */
		break;
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_action_read(struct counter_device *counter,
				    struct counter_count *count,
				    struct counter_synapse *synapse,
				    enum counter_synapse_action *action)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp = &priv->count_priv[count->id];
	enum gpio_counter_signal_id signal_id = synapse->signal->id;
	enum counter_function function;
	enum counter_count_direction direction;
	unsigned long flags;

	if (signal_id == GPIO_COUNTER_SIGNAL_INDEX1 ||
	    signal_id == GPIO_COUNTER_SIGNAL_INDEX2) {
		*action = COUNTER_SYNAPSE_ACTION_RISING_EDGE;
		return 0;
	}

	spin_lock_irqsave(&priv->lock, flags);
	function = cp->function;
	direction = cp->direction;
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
		if (signal_id == GPIO_COUNTER_SIGNAL_A)
			*action = COUNTER_SYNAPSE_ACTION_RISING_EDGE;
		return 0;

	case COUNTER_FUNCTION_INCREASE:
	case COUNTER_FUNCTION_DECREASE:
		if ((count->id == GPIO_COUNTER_COUNT_1 &&
		     signal_id == GPIO_COUNTER_SIGNAL_A) ||
		    (count->id == GPIO_COUNTER_COUNT_2 &&
		     signal_id == GPIO_COUNTER_SIGNAL_B))
			*action = COUNTER_SYNAPSE_ACTION_BOTH_EDGES;
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
	case GPIO_COUNTER_SIGNAL_INDEX1:
		gpio = priv->gpio_idx[0];
		break;
	case GPIO_COUNTER_SIGNAL_INDEX2:
		gpio = priv->gpio_idx[1];
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
	if (watch->channel >= GPIO_COUNTER_NUM_COUNTS)
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
	.count_read	= gpio_counter_count_read,
	.count_write	= gpio_counter_count_write,
	.function_read	= gpio_counter_function_read,
	.function_write	= gpio_counter_function_write,
	.action_read	= gpio_counter_action_read,
	.signal_read	= gpio_counter_signal_read,
	.watch_validate	= gpio_counter_watch_validate,
};

static int gpio_counter_ceiling_read(struct counter_device *counter,
				     struct counter_count *count, u64 *val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp = &priv->count_priv[count->id];
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*val = cp->ceiling;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_ceiling_write(struct counter_device *counter,
				      struct counter_count *count, u64 val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp = &priv->count_priv[count->id];
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	cp->ceiling = val;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_enable_read(struct counter_device *counter,
				    struct counter_count *count, u8 *enable)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp = &priv->count_priv[count->id];
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*enable = cp->enabled;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

/* enable_lock held */
static void gpio_counter_enable_b(struct gpio_counter_priv *priv)
{
	if (priv->b_irq_users++ == 0)
		enable_irq(priv->irq_b);
}

/* enable_lock held */
static void gpio_counter_disable_b(struct gpio_counter_priv *priv)
{
	if (--priv->b_irq_users == 0)
		disable_irq(priv->irq_b);
}

static void gpio_counter_enable_irqs(struct gpio_counter_priv *priv,
				     enum gpio_counter_count_id count_id)
{
	switch (count_id) {
	case GPIO_COUNTER_COUNT_1:
		enable_irq(priv->irq_a);
		gpio_counter_enable_b(priv);
		if (priv->n_idx >= 1)
			enable_irq(priv->irq_idx[0]);
		break;
	case GPIO_COUNTER_COUNT_2:
		gpio_counter_enable_b(priv);
		if (priv->n_idx >= 2)
			enable_irq(priv->irq_idx[1]);
		break;
	default:
		break;
	}
}

static void gpio_counter_disable_irqs(struct gpio_counter_priv *priv,
				      enum gpio_counter_count_id count_id)
{
	switch (count_id) {
	case GPIO_COUNTER_COUNT_1:
		if (priv->n_idx >= 1)
			disable_irq(priv->irq_idx[0]);
		gpio_counter_disable_b(priv);
		disable_irq(priv->irq_a);
		break;
	case GPIO_COUNTER_COUNT_2:
		if (priv->n_idx >= 2)
			disable_irq(priv->irq_idx[1]);
		gpio_counter_disable_b(priv);
		break;
	default:
		break;
	}
}

static int gpio_counter_enable_write(struct counter_device *counter,
				     struct counter_count *count, u8 enable)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp = &priv->count_priv[count->id];
	unsigned long flags;
	bool current_state;

	guard(mutex)(&priv->enable_lock);

	spin_lock_irqsave(&priv->lock, flags);
	current_state = cp->enabled;
	spin_unlock_irqrestore(&priv->lock, flags);

	if (current_state == enable)
		return 0;

	/* disable_irq() may sleep, so it can not run under the spinlock. */
	if (enable) {
		spin_lock_irqsave(&priv->lock, flags);
		priv->prev_a = !!gpiod_get_value(priv->gpio_a);
		priv->prev_b = !!gpiod_get_value(priv->gpio_b);
		cp->enabled = true;
		spin_unlock_irqrestore(&priv->lock, flags);
		gpio_counter_enable_irqs(priv, count->id);
	} else {
		spin_lock_irqsave(&priv->lock, flags);
		cp->enabled = false;
		spin_unlock_irqrestore(&priv->lock, flags);
		gpio_counter_disable_irqs(priv, count->id);
	}

	return 0;
}

static int gpio_counter_direction_read(struct counter_device *counter,
				       struct counter_count *count,
				       u32 *direction)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp = &priv->count_priv[count->id];
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*direction = cp->direction;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_preset_read(struct counter_device *counter,
				    struct counter_count *count, u64 *val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp = &priv->count_priv[count->id];
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*val = cp->preset;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_preset_write(struct counter_device *counter,
				     struct counter_count *count, u64 val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp = &priv->count_priv[count->id];
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&priv->lock, flags);
	if (val > cp->ceiling) {
		ret = -EINVAL;
		goto out;
	}
	cp->preset = val;
out:
	spin_unlock_irqrestore(&priv->lock, flags);

	return ret;
}

static int gpio_counter_preset_enable_read(struct counter_device *counter,
					   struct counter_count *count,
					   u8 *val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp = &priv->count_priv[count->id];
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	*val = cp->preset_enabled;
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int gpio_counter_preset_enable_write(struct counter_device *counter,
					    struct counter_count *count,
					    u8 val)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	struct gpio_counter_count_priv *cp = &priv->count_priv[count->id];
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	cp->preset_enabled = val;
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

static const enum counter_synapse_action gpio_counter_synapse_actions[] = {
	COUNTER_SYNAPSE_ACTION_NONE,
	COUNTER_SYNAPSE_ACTION_RISING_EDGE,
	COUNTER_SYNAPSE_ACTION_FALLING_EDGE,
	COUNTER_SYNAPSE_ACTION_BOTH_EDGES,
};

static const enum counter_synapse_action gpio_counter_index_synapse_actions[] = {
	COUNTER_SYNAPSE_ACTION_RISING_EDGE,
};

static int gpio_counter_get_index_gpios(struct device *dev,
					struct gpio_counter_priv *priv)
{
	struct gpio_descs *descs;
	int i;

	descs = devm_gpiod_get_array_optional(dev, "index", GPIOD_IN);
	if (IS_ERR(descs))
		return dev_err_probe(dev, PTR_ERR(descs),
				     "failed to get index GPIOs\n");
	if (!descs) {
		priv->n_idx = 0;
		return 0;
	}

	if (descs->ndescs < 1 || descs->ndescs > GPIO_COUNTER_MAX_INDEX)
		return dev_err_probe(dev, -EINVAL,
				     "index-gpios: expected 1..%d entries, got %u\n",
				     GPIO_COUNTER_MAX_INDEX, descs->ndescs);

	for (i = 0; i < descs->ndescs; i++)
		priv->gpio_idx[i] = descs->desc[i];
	priv->n_idx = descs->ndescs;

	return 0;
}

static int gpio_counter_setup_signals(struct gpio_counter_priv *priv)
{
	priv->signals[GPIO_COUNTER_SIGNAL_A].id   = GPIO_COUNTER_SIGNAL_A;
	priv->signals[GPIO_COUNTER_SIGNAL_A].name = "Signal A";
	priv->signals[GPIO_COUNTER_SIGNAL_B].id   = GPIO_COUNTER_SIGNAL_B;
	priv->signals[GPIO_COUNTER_SIGNAL_B].name = "Signal B";

	if (priv->n_idx >= 1) {
		priv->signals[GPIO_COUNTER_SIGNAL_INDEX1].id =
			GPIO_COUNTER_SIGNAL_INDEX1;
		priv->signals[GPIO_COUNTER_SIGNAL_INDEX1].name = "Index 1";
	}
	if (priv->n_idx >= 2) {
		priv->signals[GPIO_COUNTER_SIGNAL_INDEX2].id =
			GPIO_COUNTER_SIGNAL_INDEX2;
		priv->signals[GPIO_COUNTER_SIGNAL_INDEX2].name = "Index 2";
	}

	return 2 + priv->n_idx;
}

static int gpio_counter_setup_synapses(struct gpio_counter_priv *priv)
{
	int n_c1 = 2, n_c2 = 1;

	priv->synapses_count1[0].actions_list = gpio_counter_synapse_actions;
	priv->synapses_count1[0].num_actions =
		ARRAY_SIZE(gpio_counter_synapse_actions);
	priv->synapses_count1[0].signal = &priv->signals[GPIO_COUNTER_SIGNAL_A];

	priv->synapses_count1[1].actions_list = gpio_counter_synapse_actions;
	priv->synapses_count1[1].num_actions =
		ARRAY_SIZE(gpio_counter_synapse_actions);
	priv->synapses_count1[1].signal = &priv->signals[GPIO_COUNTER_SIGNAL_B];

	if (priv->n_idx >= 1) {
		priv->synapses_count1[2].actions_list =
			gpio_counter_index_synapse_actions;
		priv->synapses_count1[2].num_actions =
			ARRAY_SIZE(gpio_counter_index_synapse_actions);
		priv->synapses_count1[2].signal =
			&priv->signals[GPIO_COUNTER_SIGNAL_INDEX1];
		n_c1 = 3;
	}

	priv->synapses_count2[0].actions_list = gpio_counter_synapse_actions;
	priv->synapses_count2[0].num_actions =
		ARRAY_SIZE(gpio_counter_synapse_actions);
	priv->synapses_count2[0].signal = &priv->signals[GPIO_COUNTER_SIGNAL_B];

	if (priv->n_idx >= 2) {
		priv->synapses_count2[1].actions_list =
			gpio_counter_index_synapse_actions;
		priv->synapses_count2[1].num_actions =
			ARRAY_SIZE(gpio_counter_index_synapse_actions);
		priv->synapses_count2[1].signal =
			&priv->signals[GPIO_COUNTER_SIGNAL_INDEX2];
		n_c2 = 2;
	}

	priv->counts[GPIO_COUNTER_COUNT_1].id = GPIO_COUNTER_COUNT_1;
	priv->counts[GPIO_COUNTER_COUNT_1].name = "Count 1";
	priv->counts[GPIO_COUNTER_COUNT_1].functions_list =
		gpio_counter_count1_functions;
	priv->counts[GPIO_COUNTER_COUNT_1].num_functions =
		ARRAY_SIZE(gpio_counter_count1_functions);
	priv->counts[GPIO_COUNTER_COUNT_1].synapses = priv->synapses_count1;
	priv->counts[GPIO_COUNTER_COUNT_1].num_synapses = n_c1;
	priv->counts[GPIO_COUNTER_COUNT_1].ext = gpio_counter_count_ext;
	priv->counts[GPIO_COUNTER_COUNT_1].num_ext =
		ARRAY_SIZE(gpio_counter_count_ext);

	priv->counts[GPIO_COUNTER_COUNT_2].id = GPIO_COUNTER_COUNT_2;
	priv->counts[GPIO_COUNTER_COUNT_2].name = "Count 2";
	priv->counts[GPIO_COUNTER_COUNT_2].functions_list =
		gpio_counter_count2_functions;
	priv->counts[GPIO_COUNTER_COUNT_2].num_functions =
		ARRAY_SIZE(gpio_counter_count2_functions);
	priv->counts[GPIO_COUNTER_COUNT_2].synapses = priv->synapses_count2;
	priv->counts[GPIO_COUNTER_COUNT_2].num_synapses = n_c2;
	priv->counts[GPIO_COUNTER_COUNT_2].ext = gpio_counter_count_ext;
	priv->counts[GPIO_COUNTER_COUNT_2].num_ext =
		ARRAY_SIZE(gpio_counter_count_ext);

	return 0;
}

static int gpio_counter_request_irqs(struct device *dev,
				     struct counter_device *counter)
{
	struct gpio_counter_priv *priv = counter_priv(counter);
	irq_handler_t idx_handlers[GPIO_COUNTER_MAX_INDEX] = {
		gpio_counter_index1_isr,
		gpio_counter_index2_isr,
	};
	const char *idx_names[GPIO_COUNTER_MAX_INDEX] = {
		"gpio-counter-index1",
		"gpio-counter-index2",
	};
	int ret, i;

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

	for (i = 0; i < priv->n_idx; i++) {
		ret = devm_request_irq(dev, priv->irq_idx[i], idx_handlers[i],
				       IRQF_TRIGGER_RISING | IRQF_NO_AUTOEN,
				       idx_names[i], counter);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to request IRQ for index %d\n",
					     i);
	}

	return 0;
}

static int gpio_counter_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct counter_device *counter;
	struct gpio_counter_priv *priv;
	int num_signals;
	int i, ret;

	counter = devm_counter_alloc(dev, sizeof(*priv));
	if (!counter)
		return -ENOMEM;

	priv = counter_priv(counter);
	spin_lock_init(&priv->lock);
	ret = devm_mutex_init(dev, &priv->enable_lock);
	if (ret)
		return ret;

	priv->gpio_a = devm_gpiod_get(dev, "signal-a", GPIOD_IN);
	if (IS_ERR(priv->gpio_a))
		return dev_err_probe(dev, PTR_ERR(priv->gpio_a),
				     "failed to get signal-a GPIO\n");

	priv->gpio_b = devm_gpiod_get(dev, "signal-b", GPIOD_IN);
	if (IS_ERR(priv->gpio_b))
		return dev_err_probe(dev, PTR_ERR(priv->gpio_b),
				     "failed to get signal-b GPIO\n");

	ret = gpio_counter_get_index_gpios(dev, priv);
	if (ret)
		return ret;

	if (gpiod_cansleep(priv->gpio_a) || gpiod_cansleep(priv->gpio_b))
		return dev_err_probe(dev, -EINVAL,
				     "signal GPIO may sleep, not usable in hardirq\n");
	for (i = 0; i < priv->n_idx; i++) {
		if (gpiod_cansleep(priv->gpio_idx[i]))
			return dev_err_probe(dev, -EINVAL,
					     "index GPIO %d may sleep, not usable in hardirq\n",
					     i);
	}

	priv->irq_a = gpiod_to_irq(priv->gpio_a);
	if (priv->irq_a < 0)
		return dev_err_probe(dev, priv->irq_a,
				     "failed to get IRQ for signal-a\n");

	priv->irq_b = gpiod_to_irq(priv->gpio_b);
	if (priv->irq_b < 0)
		return dev_err_probe(dev, priv->irq_b,
				     "failed to get IRQ for signal-b\n");

	for (i = 0; i < priv->n_idx; i++) {
		priv->irq_idx[i] = gpiod_to_irq(priv->gpio_idx[i]);
		if (priv->irq_idx[i] < 0)
			return dev_err_probe(dev, priv->irq_idx[i],
					     "failed to get IRQ for index %d\n",
					     i);
	}

	priv->prev_a = !!gpiod_get_value(priv->gpio_a);
	priv->prev_b = !!gpiod_get_value(priv->gpio_b);

	priv->count_priv[GPIO_COUNTER_COUNT_1].function =
		COUNTER_FUNCTION_QUADRATURE_X4;
	priv->count_priv[GPIO_COUNTER_COUNT_1].direction =
		COUNTER_COUNT_DIRECTION_FORWARD;
	priv->count_priv[GPIO_COUNTER_COUNT_1].ceiling = U64_MAX;

	priv->count_priv[GPIO_COUNTER_COUNT_2].function =
		COUNTER_FUNCTION_INCREASE;
	priv->count_priv[GPIO_COUNTER_COUNT_2].direction =
		COUNTER_COUNT_DIRECTION_FORWARD;
	priv->count_priv[GPIO_COUNTER_COUNT_2].ceiling = U64_MAX;

	num_signals = gpio_counter_setup_signals(priv);
	gpio_counter_setup_synapses(priv);

	counter->name		= dev_name(dev);
	counter->parent		= dev;
	counter->ops		= &gpio_counter_ops;
	counter->signals	= priv->signals;
	counter->num_signals	= num_signals;
	counter->counts		= priv->counts;
	counter->num_counts	= ARRAY_SIZE(priv->counts);

	ret = gpio_counter_request_irqs(dev, counter);
	if (ret)
		return ret;

	ret = devm_counter_add(dev, counter);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to add counter\n");

	dev_info(dev,
		 "GPIO counter registered (signals: A, B%s%s)\n",
		 priv->n_idx >= 1 ? ", Index 1" : "",
		 priv->n_idx >= 2 ? ", Index 2" : "");

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
MODULE_DESCRIPTION("GPIO-based counter driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("COUNTER");
