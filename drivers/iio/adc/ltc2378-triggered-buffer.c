// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Analog Devices, Inc.
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#include <linux/iio/buffer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

#include <ltc2378.h>

static irqreturn_t ltc2378_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ltc2378_state *st = iio_priv(indio_dev);
	int ret;

	ret = ltc2378_convert_and_acquire(st);
	if (ret < 0)
		goto err_out;

	iio_push_to_buffers_with_ts(indio_dev, &st->scan, sizeof(st->scan),
				    pf->timestamp);

err_out:
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

static int ltc2378_triggered_buffer_setup(struct iio_dev *indio_dev, struct ltc2378_state *st)
{
	return devm_iio_triggered_buffer_setup(&st->spi->dev, indio_dev,
					       &iio_pollfunc_store_time,
					       &ltc2378_trigger_handler,
					       NULL);
}

static const struct ltc2378_ops ltc2378_triggered_buf_ops = {
	.buffer_setup = ltc2378_triggered_buffer_setup,
};

int ltc2378_set_triggered_buf_ops(struct ltc2378_state *st)
{
	st->ops = &ltc2378_triggered_buf_ops;
	return 0;
}
EXPORT_SYMBOL_NS_GPL(ltc2378_set_triggered_buf_ops, "IIO_LTC2378");
