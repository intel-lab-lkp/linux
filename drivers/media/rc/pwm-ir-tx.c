// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2017 Sean Young <sean@mess.org>
 */

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/pwm.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <media/rc-core.h>

#define DRIVER_NAME	"pwm-ir-tx"
#define DEVICE_NAME	"PWM IR Transmitter"

struct pwm_ir {
	struct pwm_device *pwm;
	struct hrtimer timer;
	struct task_struct *tx_thread;
	wait_queue_head_t tx_wq;
	struct completion tx_done;
	struct completion edge;
	unsigned int carrier;
	unsigned int duty_cycle;
	unsigned int *txbuf;
	unsigned int count;
	unsigned int index;
};

static const struct of_device_id pwm_ir_of_match[] = {
	{ .compatible = "pwm-ir-tx", },
	{ },
};
MODULE_DEVICE_TABLE(of, pwm_ir_of_match);

static int pwm_ir_set_duty_cycle(struct rc_dev *dev, u32 duty_cycle)
{
	struct pwm_ir *pwm_ir = dev->priv;

	pwm_ir->duty_cycle = duty_cycle;

	return 0;
}

static int pwm_ir_set_carrier(struct rc_dev *dev, u32 carrier)
{
	struct pwm_ir *pwm_ir = dev->priv;

	if (!carrier)
		return -EINVAL;

	pwm_ir->carrier = carrier;

	return 0;
}

static enum hrtimer_restart pwm_ir_timer_cb(struct hrtimer *timer)
{
	struct pwm_ir *pwm_ir = container_of(timer, struct pwm_ir, timer);
	ktime_t now;

	/*
	 * If we happen to hit an odd latency spike, loop through the
	 * pulses until we catch up.
	 */
	do {
		u64 edge;

		complete(&pwm_ir->edge);

		if (pwm_ir->index >= pwm_ir->count)
			return HRTIMER_NORESTART;

		edge = US_TO_NS(pwm_ir->txbuf[pwm_ir->index]);
		hrtimer_add_expires_ns(timer, edge);

		pwm_ir->index++;

		now = timer->base->get_time();

	} while (hrtimer_get_expires_tv64(timer) < now);

	return HRTIMER_RESTART;
}

static void _pwm_ir_tx(struct pwm_ir *pwm_ir)
{
	struct pwm_device *pwm = pwm_ir->pwm;
	struct pwm_state state;
	unsigned int *txbuf = pwm_ir->txbuf;
	unsigned int count = pwm_ir->count;
	int i;
	ktime_t edge;
	long delta;

	pwm_init_state(pwm, &state);

	state.period = DIV_ROUND_CLOSEST(NSEC_PER_SEC, pwm_ir->carrier);
	pwm_set_relative_duty_cycle(&state, pwm_ir->duty_cycle, 100);

	hrtimer_start(&pwm_ir->timer, 0, HRTIMER_MODE_REL);
	wait_for_completion(&pwm_ir->edge);
	edge = ktime_get();

	for (i = 0; i < count; i++) {
		state.enabled = !(i % 2);
		pwm_apply_state(pwm, &state);

		edge = ktime_add_us(edge, txbuf[i]);
		wait_for_completion(&pwm_ir->edge);

		delta = ktime_us_delta(edge, ktime_get());

		if (delta > 0)
			udelay(delta);
	}

	state.enabled = false;
	pwm_apply_state(pwm, &state);

	pwm_ir->count = 0;
}

static int pwm_ir_thread(void *data)
{
	struct pwm_ir *pwm_ir = data;

	for (;;) {
		wait_event_idle(pwm_ir->tx_wq,
				kthread_should_stop() || pwm_ir->count);

		if (kthread_should_stop())
			break;

		_pwm_ir_tx(pwm_ir);
		complete(&pwm_ir->tx_done);
	}

	return 0;
}

static int pwm_ir_tx(struct rc_dev *dev, unsigned int *txbuf,
		     unsigned int count)
{
	struct pwm_ir *pwm_ir = dev->priv;

	pwm_ir->txbuf = txbuf;
	pwm_ir->count = count;
	pwm_ir->index = 0;

	wake_up(&pwm_ir->tx_wq);
	wait_for_completion(&pwm_ir->tx_done);

	return count;
}

static int pwm_ir_probe(struct platform_device *pdev)
{
	struct pwm_ir *pwm_ir;
	struct rc_dev *rcdev;
	int rc;

	pwm_ir = devm_kmalloc(&pdev->dev, sizeof(*pwm_ir), GFP_KERNEL);
	if (!pwm_ir)
		return -ENOMEM;

	platform_set_drvdata(pdev, pwm_ir);

	pwm_ir->pwm = devm_pwm_get(&pdev->dev, NULL);
	if (IS_ERR(pwm_ir->pwm))
		return PTR_ERR(pwm_ir->pwm);

	/* Use default, in case userspace does not set the carrier */
	pwm_ir->carrier = DIV_ROUND_CLOSEST_ULL(pwm_get_period(pwm_ir->pwm),
						NSEC_PER_SEC);
	pwm_ir->duty_cycle = 50;
	pwm_ir->count = 0;

	init_waitqueue_head(&pwm_ir->tx_wq);
	init_completion(&pwm_ir->edge);
	init_completion(&pwm_ir->tx_done);

	hrtimer_init(&pwm_ir->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	pwm_ir->timer.function = pwm_ir_timer_cb;

	rcdev = devm_rc_allocate_device(&pdev->dev, RC_DRIVER_IR_RAW_TX);
	if (!rcdev)
		return -ENOMEM;

	rcdev->priv = pwm_ir;
	rcdev->driver_name = DRIVER_NAME;
	rcdev->device_name = DEVICE_NAME;
	rcdev->tx_ir = pwm_ir_tx;
	rcdev->s_tx_duty_cycle = pwm_ir_set_duty_cycle;
	rcdev->s_tx_carrier = pwm_ir_set_carrier;

	rc = devm_rc_register_device(&pdev->dev, rcdev);
	if (rc < 0) {
		dev_err(&pdev->dev, "failed to register rc device\n");
		return rc;
	}

	pwm_ir->tx_thread = kthread_create(pwm_ir_thread, pwm_ir, "%s/tx",
					   dev_name(&pdev->dev));
	if (IS_ERR(pwm_ir->tx_thread))
		return PTR_ERR(pwm_ir->tx_thread);

	sched_set_fifo(pwm_ir->tx_thread);
	wake_up_process(pwm_ir->tx_thread);

	return 0;
}

static int pwm_ir_remove(struct platform_device *pdev)
{
	struct pwm_ir *pwm_ir = platform_get_drvdata(pdev);

	kthread_stop(pwm_ir->tx_thread);

	return 0;
}

static struct platform_driver pwm_ir_driver = {
	.probe = pwm_ir_probe,
	.remove = pwm_ir_remove,
	.driver = {
		.name	= DRIVER_NAME,
		.of_match_table = pwm_ir_of_match,
	},
};
module_platform_driver(pwm_ir_driver);

MODULE_DESCRIPTION("PWM IR Transmitter");
MODULE_AUTHOR("Sean Young <sean@mess.org>");
MODULE_LICENSE("GPL");
