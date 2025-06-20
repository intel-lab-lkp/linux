// SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
/**
 * @section LICENSE
 * Copyright (c) 2024 Bosch Sensortec GmbH All Rights Reserved.
 *
 * This software program is licensed subject to the GNU General
 * Public License (GPL).Version 2,June 1991,
 * available at http://www.fsf.org/copyleft/gpl.html
 *
 * @file		bmp390_iio_buffer.c
 * @date		2025-06-02
 * @version		v2.2.0
 *
 * @brief		BMP390 Linux Driver IIO Buffer Source
 *
 */

#include "bmp390_driver.h"

static int iio_trig_hrtimer_set_state(struct iio_trigger *trig, bool state);
/**
 * bmp390_iio_trigger_h() - the trigger handler function
 * @irq: the interrupt number
 * @p: private data - always a pointer to the poll func.
 *
 */
static irqreturn_t bmp390_iio_trigger_h(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	u64 *iio_data;
	unsigned int i, j;
	int rslt;
	struct bmp3_client_data *client_data = iio_priv(indio_dev);
	struct bmp3_data sensor_data = { 0 };

	iio_data = kmalloc(indio_dev->scan_bytes, GFP_KERNEL);
	if (!iio_data)
		goto done;

	if (!bitmap_empty(indio_dev->active_scan_mask, indio_dev->masklength)) {
		for (i = 0, j = 0;
		     i < bitmap_weight(indio_dev->active_scan_mask,
				       indio_dev->masklength);
		     i++, j++) {
			j = find_next_bit(indio_dev->active_scan_mask,
					  indio_dev->masklength, j);

			rslt = bmp3_get_sensor_data(BMP3_PRESS_TEMP,
						    &sensor_data,
						    &client_data->device);
			if (rslt)
				pr_err("Failed to get sensor data %d\n", rslt);
			iio_data[0] = (int64_t)sensor_data.pressure;
			iio_data[1] = sensor_data.temperature;
		}
	}
	/*lint -e534*/
	iio_push_to_buffers_with_timestamp(indio_dev, iio_data,
					   iio_get_time_ns(indio_dev));
	/*lint +e534*/
	kfree(iio_data);
done:
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static const struct iio_buffer_setup_ops iio_bmp390_buffer_setup_ops = {
};

/**
 * bmp390_iio_configure_buffer() - register buffer resources
 * @indo_dev: device instance state
 */
int bmp390_iio_configure_buffer(struct iio_dev *indio_dev)
{
	int ret;
	struct iio_buffer *buffer;

	buffer = iio_kfifo_allocate();
	if (!buffer) {
		ret = -ENOMEM;
		goto error_ret;
	}

	(void)iio_device_attach_buffer(indio_dev, buffer);

	indio_dev->setup_ops = &iio_bmp390_buffer_setup_ops;
	indio_dev->pollfunc = iio_alloc_pollfunc(NULL,
						 bmp390_iio_trigger_h,
						 IRQF_ONESHOT,
						 indio_dev,
						 "%s-dev%d", indio_dev->name,
						 iio_device_id(indio_dev));
	if (!indio_dev->pollfunc) {
		ret = -ENOMEM;
		goto error_free_buffer;
	}

	indio_dev->modes |= INDIO_BUFFER_TRIGGERED;

	return 0;

error_free_buffer:
	iio_dealloc_pollfunc(indio_dev->pollfunc);
error_ret:
	iio_kfifo_free(indio_dev->buffer);
	return ret;
}

/**
 * bmp390_iio_unconfigure_buffer() - release buffer resources
 * @indo_dev: device instance state
 */
void bmp390_iio_unconfigure_buffer(struct iio_dev *indio_dev)
{
	iio_dealloc_pollfunc(indio_dev->pollfunc);
	iio_kfifo_free(indio_dev->buffer);
}

static enum hrtimer_restart iio_hrtimer_trig_handler(struct hrtimer *timer)
{
	/*lint -e26 -e10 -e516 -e124 -e40 -e831 -e64 -e119 -e413 -e534*/
	struct iio_hrtimer_info *info = container_of(timer,
		struct iio_hrtimer_info, timer);
	/*lint +e26  +e10 +e516 +e124 +e40 +e831 +e64 +e119 +e413 +e534*/
	/*lint -e534*/
	hrtimer_forward_now(timer, info->period);
	/*lint +e534*/
	iio_trigger_poll(info->swt.trigger);

	return HRTIMER_RESTART;
}

static int iio_trig_hrtimer_set_state(struct iio_trigger *trig, bool state)
{
	struct iio_hrtimer_info *trig_info;

	trig_info = iio_trigger_get_drvdata(trig);
	if (state)
		hrtimer_start(&trig_info->timer, trig_info->period,
			      HRTIMER_MODE_REL_HARD);
	else
		/*lint -e534*/
		hrtimer_cancel(&trig_info->timer);
		/*lint +e534*/

	return 0;
}

static const struct iio_trigger_ops iio_hrtimer_trigger_ops = {
	/*lint -e546*/
	.set_trigger_state = iio_trig_hrtimer_set_state,
	/*lint +e546*/
};

/**
 * bmp390_iio_allocate_trigger() - register trigger resources
 * @indo_dev: device instance state
 */
int bmp390_iio_allocate_trigger(struct iio_dev *indio_dev)
{
	struct bmp3_client_data *sdata = iio_priv(indio_dev);
	int ret = 0;

	if (!sdata->trig_info) {
		sdata->trig_info = kzalloc(sizeof(*sdata->trig_info), GFP_KERNEL);
		if (!sdata->trig_info)
			return -ENOMEM;

		sdata->trig_info->swt.trigger =
		iio_trigger_alloc(indio_dev->dev.parent,
				  "%s-dev%d",
				  indio_dev->name,
				  iio_device_id(indio_dev));
		if (!sdata->trig_info->swt.trigger) {
			ret = -ENOMEM;
			goto err_free_trig_info;
		}
		iio_trigger_set_drvdata(sdata->trig_info->swt.trigger,
					sdata->trig_info);
		sdata->trig_info->swt.trigger->ops = &iio_hrtimer_trigger_ops;
		hrtimer_setup(&sdata->trig_info->timer, iio_hrtimer_trig_handler,
			      CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
		sdata->trig_info->sampling_frequency =
								HRTIMER_DEFAULT_SAMPLING_FREQUENCY;
		sdata->trig_info->period = ns_to_ktime(10000000);
		ret = iio_trigger_register(sdata->trig_info->swt.trigger);
		if (ret)
			goto err_free_trigger;
	}
	return ret;
err_free_trigger:
	iio_trigger_free(sdata->trig_info->swt.trigger);
err_free_trig_info:
	kfree(sdata->trig_info);
	return ret;
}

/**
 * bmp390_iio_deallocate_trigger() - release trigger resources
 * @indo_dev: device instance state
 */
void bmp390_iio_deallocate_trigger(struct iio_dev *indio_dev)
{
	struct bmp3_client_data *sdata = iio_priv(indio_dev);

	if (sdata->trig_info) {
		iio_trigger_unregister(sdata->trig_info->swt.trigger);
		/* cancel the timer after unreg to make sure no one rearms it */
		/*lint -e534*/
		hrtimer_cancel(&sdata->trig_info->timer);
		/*lint +e534*/
		iio_trigger_free(sdata->trig_info->swt.trigger);
		kfree(sdata->trig_info);
		sdata->trig_info = NULL;
	}
}

