// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Invensense, Inc.
 * Copyright (C) 2026 Axis Communications AB
 */

#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/delay.h>

#include <linux/iio/common/inv_sensors_timestamp.h>
#include <linux/iio/iio.h>

#include "inv_icm42370.h"
#include "inv_icm42370_buffer.h"

/* FIFO header: 1 byte */
#define INV_ICM42370_FIFO_HEADER_MSG BIT(7)
#define INV_ICM42370_FIFO_HEADER_ACCEL BIT(6)
#define INV_ICM42370_FIFO_HEADER_ODR_ACCEL BIT(1)

struct inv_icm42370_fifo_packet_1 {
	u8 header;
	struct inv_icm42370_fifo_sensor_data data;
	s8 temp;
} __packed;

#define INV_ICM42370_FIFO_PACKET_1_SIZE 8

ssize_t inv_icm42370_fifo_decode_packet(const void *packet, const void **accel,
					const s8 **temp, const void **timestamp,
					unsigned int *odr)
{
	const struct inv_icm42370_fifo_packet_1 *pack1 = packet;
	u8 header = *((const u8 *)packet);

	/* FIFO empty */
	if (header & INV_ICM42370_FIFO_HEADER_MSG) {
		*accel = NULL;
		*temp = NULL;
		*timestamp = NULL;
		*odr = 0;
		return 0;
	}

	/* ODR change flag */
	*odr = 0;
	if (header & INV_ICM42370_FIFO_HEADER_ODR_ACCEL)
		*odr |= INV_ICM42370_SENSOR_ACCEL;

	/* With TMST_EN disabled, all packets are Packet 1 (8 bytes) */
	if (header & INV_ICM42370_FIFO_HEADER_ACCEL) {
		*accel = &pack1->data;
		*temp = &pack1->temp;
		*timestamp = NULL;
		return INV_ICM42370_FIFO_PACKET_1_SIZE;
	}

	/* invalid or unsupported packet format */
	return -EINVAL;
}

void inv_icm42370_buffer_update_fifo_period(struct inv_icm42370_data *data)
{
	u32 period_accel;

	if (data->fifo.en & INV_ICM42370_SENSOR_ACCEL)
		period_accel = inv_icm42370_odr_to_period(data->conf.odr);
	else
		period_accel = U32_MAX;

	data->fifo.period = period_accel;
}

int inv_icm42370_buffer_set_fifo_en(struct inv_icm42370_data *data,
				    unsigned int fifo_en)
{
	u8 mask, val, regval;
	int ret;

	/* update only FIFO EN bits */
	mask = INV_ICM42370_FIFO_CONFIG5_ACCEL_EN;

	val = 0;
	if (fifo_en & INV_ICM42370_SENSOR_ACCEL)
		val |= INV_ICM42370_FIFO_CONFIG5_ACCEL_EN;

	ret = inv_icm42370_mreg_read(data, INV_ICM42370_MREG1,
				     INV_ICM42370_REG_FIFO_CONFIG5, &regval);
	if (ret)
		return ret;

	/* clear the mask bits and set the new values */
	regval &= ~mask;
	regval |= val;

	ret = inv_icm42370_mreg_write(data, INV_ICM42370_MREG1,
				      INV_ICM42370_REG_FIFO_CONFIG5, regval);
	if (ret)
		return ret;

	data->fifo.en = fifo_en;
	inv_icm42370_buffer_update_fifo_period(data);

	return 0;
}

static size_t inv_icm42370_get_packet_size(unsigned int fifo_en)
{
	/*
	 * With TMST_EN disabled, the device always produces Packet 1
	 * (8 bytes: 1 header + 6 accel + 1 temp).
	 */
	return INV_ICM42370_FIFO_PACKET_1_SIZE;
}

static unsigned int inv_icm42370_wm_truncate(unsigned int watermark,
					     size_t packet_size)
{
	size_t wm_size;
	unsigned int wm;

	wm_size = watermark * packet_size;
	if (wm_size > INV_ICM42370_FIFO_WATERMARK_MAX)
		wm_size = INV_ICM42370_FIFO_WATERMARK_MAX;

	wm = wm_size / packet_size;

	return wm;
}

/**
 * inv_icm42370_buffer_update_watermark - update watermark FIFO threshold
 * @data:	driver internal state
 *
 * Returns 0 on success, a negative error code otherwise.
 *
 * FIFO watermark threshold is computed based on the required
 * watermark values set for accel sensor.
 */
int inv_icm42370_buffer_update_watermark(struct inv_icm42370_data *data)
{
	size_t packet_size, wm_size;
	unsigned int wm, watermark;
	bool restore;
	__le16 raw_wm;
	int ret;

	packet_size = inv_icm42370_get_packet_size(data->fifo.en);

	/* compute sensors latency, depending on sensor watermark and odr */
	wm = inv_icm42370_wm_truncate(data->fifo.watermark.accel, packet_size);

	/* 0 value for watermark means that the sensor is turned off */
	if (wm == 0)
		return 0;

	watermark = wm;
	data->fifo.watermark.eff_accel = wm;

	/* compute watermark value in bytes */
	wm_size = watermark * packet_size;

	/* changing FIFO watermark requires to turn off watermark interrupt */
	ret = regmap_update_bits_check(
		data->map, INV_ICM42370_REG_INT_SOURCE0,
		INV_ICM42370_INT_SOURCE0_FIFO_THS_INT1_EN, 0, &restore);
	if (ret)
		return ret;

	raw_wm = INV_ICM42370_FIFO_WATERMARK_VAL(wm_size);
	memcpy(data->buffer, &raw_wm, sizeof(raw_wm));
	ret = regmap_bulk_write(data->map, INV_ICM42370_REG_FIFO_WATERMARK,
				data->buffer, sizeof(raw_wm));
	if (ret)
		return ret;

	/* restore watermark interrupt */
	if (restore) {
		ret = regmap_set_bits(
			data->map, INV_ICM42370_REG_INT_SOURCE0,
			INV_ICM42370_INT_SOURCE0_FIFO_THS_INT1_EN);
		if (ret)
			return ret;
	}

	return 0;
}

static int inv_icm42370_buffer_preenable(struct iio_dev *indio_dev)
{
	struct inv_icm42370_data *data = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(data->map);
	struct inv_sensors_timestamp *ts = &data->ts;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	guard(mutex)(&data->lock);

	inv_sensors_timestamp_reset(ts);

	return 0;
}

static int inv_icm42370_buffer_postenable(struct iio_dev *indio_dev)
{
	struct inv_icm42370_data *data = iio_priv(indio_dev);
	int ret;

	guard(mutex)(&data->lock);

	/* Exit if FIFO is already on. */
	if (data->fifo.on) {
		data->fifo.on++;
		return 0;
	}

	ret = regmap_write(data->map, INV_ICM42370_REG_SIGNAL_PATH_RESET,
			   INV_ICM42370_SIGNAL_PATH_RESET_FIFO_FLUSH);
	if (ret)
		return ret;

	ret = regmap_write(data->map, INV_ICM42370_REG_FIFO_CONFIG1,
			   INV_ICM42370_FIFO_CONFIG_STREAM);
	if (ret)
		return ret;

	/* when FIFO_CONFIG_STREAM bit is set FIFO is enabled, so
	 * increase the count
	 */
	data->fifo.on++;

	ret = regmap_bulk_read(data->map, INV_ICM42370_REG_FIFO_COUNT, data->buffer,
			       2);
	if (ret)
		return ret;

	ret = regmap_set_bits(data->map, INV_ICM42370_REG_INT_SOURCE0,
			      INV_ICM42370_INT_SOURCE0_FIFO_THS_INT1_EN);
	if (ret)
		return ret;

	return 0;
}

static int inv_icm42370_buffer_predisable(struct iio_dev *indio_dev)
{
	struct inv_icm42370_data *data = iio_priv(indio_dev);
	int ret;

	guard(mutex)(&data->lock);

	/* Exit if there are several sensors using the FIFO. */
	if (data->fifo.on > 1) {
		data->fifo.on--;
		return 0;
	}

	/* set FIFO in bypass mode */
	ret = regmap_write(data->map, INV_ICM42370_REG_FIFO_CONFIG1,
			   INV_ICM42370_FIFO_CONFIG_BYPASS);
	if (ret)
		return ret;

	/* when FIFO is bypassed it gets disabled, so reduce the
	 * count
	 */
	data->fifo.on--;

	/* flush FIFO data */
	ret = regmap_write(data->map, INV_ICM42370_REG_SIGNAL_PATH_RESET,
			   INV_ICM42370_SIGNAL_PATH_RESET_FIFO_FLUSH);
	if (ret)
		return ret;

	/* disable FIFO threshold interrupt */
	ret = regmap_clear_bits(data->map, INV_ICM42370_REG_INT_SOURCE0,
				INV_ICM42370_INT_SOURCE0_FIFO_THS_INT1_EN);
	if (ret)
		return ret;

	return 0;
}

static int inv_icm42370_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct inv_icm42370_data *data = iio_priv(indio_dev);
	struct inv_sensors_timestamp *ts = &data->ts;
	struct device *dev = regmap_get_device(data->map);
	unsigned int sensor;
	unsigned int *watermark;
	struct inv_icm42370_conf conf = INV_ICM42370_SENSOR_CONF_INIT;
	unsigned int sleep_temp = 0;
	unsigned int sleep_sensor = 0;
	unsigned int sleep;
	int ret;

	if (indio_dev == data->indio_accel) {
		sensor = INV_ICM42370_SENSOR_ACCEL;
		watermark = &data->fifo.watermark.accel;
	} else {
		return -EINVAL;
	}

	guard(mutex)(&data->lock);

	inv_sensors_timestamp_apply_odr(ts, 0, 0, 0);

	ret = inv_icm42370_buffer_set_fifo_en(data, data->fifo.en & ~sensor);
	if (ret)
		goto out_unlock;

	*watermark = 0;
	ret = inv_icm42370_buffer_update_watermark(data);
	if (ret)
		goto out_unlock;

	conf.mode = INV_ICM42370_SENSOR_MODE_OFF;
	ret = inv_icm42370_set_accel_conf(data, &conf, &sleep_sensor);
	if (ret)
		goto out_unlock;

out_unlock:
	/* sleep maximum required time */
	sleep = max(sleep_sensor, sleep_temp);
	if (sleep)
		msleep(sleep);

	pm_runtime_put_autosuspend(dev);

	return ret;
}

const struct iio_buffer_setup_ops inv_icm42370_buffer_ops = {
	.preenable = inv_icm42370_buffer_preenable,
	.postenable = inv_icm42370_buffer_postenable,
	.predisable = inv_icm42370_buffer_predisable,
	.postdisable = inv_icm42370_buffer_postdisable,
};

int inv_icm42370_buffer_fifo_read(struct inv_icm42370_data *data,
				  unsigned int max)
{
	const ssize_t packet_size = sizeof(struct inv_icm42370_fifo_packet_1);
	__be16 *raw_fifo_count;
	size_t fifo_nb, i;
	ssize_t size;
	const void *accel, *timestamp;
	const s8 *temp;
	unsigned int odr;
	int ret;

	/* reset all samples counters */
	data->fifo.count = 0;
	data->fifo.nb.accel = 0;
	data->fifo.nb.total = 0;

	raw_fifo_count = (__be16 *)data->buffer;
	ret = regmap_bulk_read(data->map, INV_ICM42370_REG_FIFO_COUNT,
			       raw_fifo_count, sizeof(*raw_fifo_count));
	if (ret)
		return ret;

	/* Check and limit number of samples if requested. */
	fifo_nb = le16_to_cpup(raw_fifo_count);
	if (fifo_nb == 0)
		return 0;
	if (max > 0 && fifo_nb > max)
		fifo_nb = max;

	/*
	 * Read all FIFO data into the internal buffer, clamping the
	 * device-reported count to the buffer capacity.
	 */
	data->fifo.count = min(fifo_nb * packet_size, INV_ICM42370_FIFO_SIZE_MAX);
	ret = regmap_noinc_read(data->map, INV_ICM42370_REG_FIFO_DATA,
				data->fifo.data, data->fifo.count);
	if (ret == -EOPNOTSUPP || ret == -EFBIG) {
		/* Read full fifo is not supported, read samples one by one. */
		ret = 0;
		for (i = 0; i < data->fifo.count && ret == 0; i += packet_size)
			ret = regmap_noinc_read(data->map, INV_ICM42370_REG_FIFO_DATA,
						&data->fifo.data[i], packet_size);
	}
	if (ret)
		return ret;

	for (i = 0; i < data->fifo.count; i += size) {
		size = inv_icm42370_fifo_decode_packet(
			&data->fifo.data[i], &accel, &temp, &timestamp, &odr);
		if (size <= 0)
			/* No more sample in buffer */
			break;
		if (accel && inv_icm42370_fifo_is_data_valid(accel))
			data->fifo.nb.accel++;
		data->fifo.nb.total++;
	}

	return 0;
}

int inv_icm42370_buffer_fifo_parse(struct inv_icm42370_data *data)
{
	struct inv_sensors_timestamp *ts;
	int ret;

	if (data->fifo.nb.total == 0)
		return 0;

	/* handle accelerometer timestamp and FIFO data parsing */
	if (data->fifo.nb.accel > 0) {
		ts = &data->ts;
		inv_sensors_timestamp_interrupt(
			ts, data->fifo.watermark.eff_accel, data->timestamp);
		ret = inv_icm42370_accel_parse_fifo(data->indio_accel);
		if (ret)
			return ret;
	}

	return 0;
}

int inv_icm42370_buffer_hwfifo_flush(struct inv_icm42370_data *data,
				     unsigned int count)
{
	struct inv_sensors_timestamp *ts;
	s64 accel_ts;
	int ret;

	accel_ts = iio_get_time_ns(data->indio_accel);

	ret = inv_icm42370_buffer_fifo_read(data, count);
	if (ret)
		return ret;

	if (data->fifo.nb.total == 0)
		return 0;

	if (data->fifo.nb.accel > 0) {
		ts = &data->ts;
		inv_sensors_timestamp_interrupt(ts, data->fifo.nb.accel,
						accel_ts);
		ret = inv_icm42370_accel_parse_fifo(data->indio_accel);
		if (ret)
			return ret;
	}

	return 0;
}

int inv_icm42370_buffer_init(struct inv_icm42370_data *data)
{
	unsigned int val;
	u8 regval;
	int ret;

	data->fifo.watermark.eff_accel = 1;

	/* watermark should be set to a non-zero value before enabling interrupts */
	data->fifo.watermark.accel = 1;
	ret = inv_icm42370_buffer_update_watermark(data);
	if (ret)
		return ret;

	/*
	 * Default FIFO configuration (bits 6 to 5)
	 * - FIFO count in bytes
	 * - FIFO count in big endian
	 */
	val = INV_ICM42370_INTF_CONFIG0_FIFO_COUNT_ENDIAN;
	ret = regmap_update_bits(data->map, INV_ICM42370_REG_INTF_CONFIG0,
				 GENMASK(6, 5), val);
	if (ret)
		return ret;

	/*
	 * Enable FIFO partial read interrupt.
	 * Disable all FIFO EN bits.
	 */
	ret = inv_icm42370_mreg_read(data, INV_ICM42370_MREG1,
				     INV_ICM42370_REG_FIFO_CONFIG5, &regval);
	if (ret)
		return ret;

	regval &= ~(GENMASK(6, 5) | GENMASK(3, 0));
	regval |= INV_ICM42370_FIFO_CONFIG5_WM_GT_TH;
	regval |= INV_ICM42370_FIFO_CONFIG5_RESUME_PARTIAL_RD;

	return inv_icm42370_mreg_write(data, INV_ICM42370_MREG1,
				       INV_ICM42370_REG_FIFO_CONFIG5, regval);
}
