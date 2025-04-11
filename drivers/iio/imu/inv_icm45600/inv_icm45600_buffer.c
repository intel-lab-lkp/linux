// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Invensense, Inc.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/iio/buffer.h>
#include <linux/iio/common/inv_sensors_timestamp.h>
#include <linux/iio/iio.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include "inv_icm45600_buffer.h"
#include "inv_icm45600.h"

/* FIFO header: 1 byte */
#define INV_ICM45600_FIFO_EXT_HEADER		BIT(7)
#define INV_ICM45600_FIFO_HEADER_ACCEL		BIT(6)
#define INV_ICM45600_FIFO_HEADER_GYRO		BIT(5)
#define INV_ICM45600_FIFO_HEADER_HIGH_RES	BIT(4)
#define INV_ICM45600_FIFO_HEADER_TMST_FSYNC	GENMASK(3, 2)
#define INV_ICM45600_FIFO_HEADER_ODR_ACCEL	BIT(1)
#define INV_ICM45600_FIFO_HEADER_ODR_GYRO	BIT(0)

struct inv_icm45600_fifo_1sensor_packet {
	uint8_t header;
	struct inv_icm45600_fifo_sensor_data data;
	int8_t temp;
} __packed;
#define INV_ICM45600_FIFO_1SENSOR_PACKET_SIZE		8

struct inv_icm45600_fifo_2sensors_packet {
	uint8_t header;
	struct inv_icm45600_fifo_sensor_data accel;
	struct inv_icm45600_fifo_sensor_data gyro;
	int8_t temp;
	__le16 timestamp;
} __packed;
#define INV_ICM45600_FIFO_2SENSORS_PACKET_SIZE		16

ssize_t inv_icm45600_fifo_decode_packet(const void *packet, const void **accel,
					const void **gyro, const int8_t **temp,
					const void **timestamp, unsigned int *odr)
{
	const struct inv_icm45600_fifo_1sensor_packet *pack1 = packet;
	const struct inv_icm45600_fifo_2sensors_packet *pack2 = packet;
	uint8_t header = *((const uint8_t *)packet);

	/* FIFO extended header */
	if (header & INV_ICM45600_FIFO_EXT_HEADER) {
		/* Not yet supported */
		return 0;
	}

	/* handle odr flags */
	*odr = 0;
	if (header & INV_ICM45600_FIFO_HEADER_ODR_GYRO)
		*odr |= INV_ICM45600_SENSOR_GYRO;
	if (header & INV_ICM45600_FIFO_HEADER_ODR_ACCEL)
		*odr |= INV_ICM45600_SENSOR_ACCEL;

	/* accel + gyro */
	if ((header & INV_ICM45600_FIFO_HEADER_ACCEL) &&
	    (header & INV_ICM45600_FIFO_HEADER_GYRO)) {
		*accel = &pack2->accel;
		*gyro = &pack2->gyro;
		*temp = &pack2->temp;
		*timestamp = &pack2->timestamp;
		return INV_ICM45600_FIFO_2SENSORS_PACKET_SIZE;
	}

	/* accel only */
	if (header & INV_ICM45600_FIFO_HEADER_ACCEL) {
		*accel = &pack1->data;
		*gyro = NULL;
		*temp = &pack1->temp;
		*timestamp = NULL;
		return INV_ICM45600_FIFO_1SENSOR_PACKET_SIZE;
	}

	/* gyro only */
	if (header & INV_ICM45600_FIFO_HEADER_GYRO) {
		*accel = NULL;
		*gyro = &pack1->data;
		*temp = &pack1->temp;
		*timestamp = NULL;
		return INV_ICM45600_FIFO_1SENSOR_PACKET_SIZE;
	}

	/* invalid packet if here */
	return -EINVAL;
}

void inv_icm45600_buffer_update_fifo_period(struct inv_icm45600_state *st)
{
	uint32_t period_gyro, period_accel, period;

	if (st->fifo.en & INV_ICM45600_SENSOR_GYRO)
		period_gyro = inv_icm45600_odr_to_period(st->conf.gyro.odr);
	else
		period_gyro = U32_MAX;

	if (st->fifo.en & INV_ICM45600_SENSOR_ACCEL)
		period_accel = inv_icm45600_odr_to_period(st->conf.accel.odr);
	else
		period_accel = U32_MAX;

	if (period_gyro <= period_accel)
		period = period_gyro;
	else
		period = period_accel;

	st->fifo.period = period;
}

int inv_icm45600_buffer_set_fifo_en(struct inv_icm45600_state *st,
				    unsigned int fifo_en)
{
	unsigned int mask, val;
	int ret;

	/* update only FIFO EN bits */
	mask = INV_ICM45600_FIFO_CONFIG3_GYRO_EN |
	       INV_ICM45600_FIFO_CONFIG3_ACCEL_EN;

	val = 0;
	if ((fifo_en & INV_ICM45600_SENSOR_GYRO) || (fifo_en & INV_ICM45600_SENSOR_ACCEL))
		val = (INV_ICM45600_FIFO_CONFIG3_GYRO_EN | INV_ICM45600_FIFO_CONFIG3_ACCEL_EN);

	ret = regmap_update_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG3, mask, val);
	if (ret)
		return ret;

	st->fifo.en = fifo_en;
	inv_icm45600_buffer_update_fifo_period(st);

	return 0;
}

static unsigned int inv_icm45600_wm_truncate(unsigned int watermark, size_t packet_size,
					     unsigned int fifo_period)
{
	size_t watermark_max, grace_samples;

	/* keep 20ms for processing FIFO */
	grace_samples = (20U * 1000000U) / fifo_period;
	if (grace_samples < 1)
		grace_samples = 1;

	watermark_max = INV_ICM45600_FIFO_SIZE_MAX / packet_size;
	watermark_max -= grace_samples;

	if (watermark > watermark_max)
		watermark = watermark_max;

	return watermark;
}

/**
 * inv_icm45600_buffer_update_watermark - update watermark FIFO threshold
 * @st:	driver internal state
 *
 * Returns 0 on success, a negative error code otherwise.
 *
 * FIFO watermark threshold is computed based on the required watermark values
 * set for gyro and accel sensors. Since watermark is all about acceptable data
 * latency, use the smallest setting between the 2. It means choosing the
 * smallest latency but this is not as simple as choosing the smallest watermark
 * value. Latency depends on watermark and ODR. It requires several steps:
 * 1) compute gyro and accel latencies and choose the smallest value.
 * 2) adapt the chosen latency so that it is a multiple of both gyro and accel
 *    ones. Otherwise it is possible that you don't meet a requirement. (for
 *    example with gyro @100Hz wm 4 and accel @100Hz with wm 6, choosing the
 *    value of 4 will not meet accel latency requirement because 6 is not a
 *    multiple of 4. You need to use the value 2.)
 * 3) Since all periods are multiple of each others, watermark is computed by
 *    dividing this computed latency by the smallest period, which corresponds
 *    to the FIFO frequency.
 */
int inv_icm45600_buffer_update_watermark(struct inv_icm45600_state *st)
{
	const size_t packet_size = INV_ICM45600_FIFO_2SENSORS_PACKET_SIZE;
	unsigned int wm_gyro, wm_accel, watermark;
	uint32_t period_gyro, period_accel, period;
	uint32_t latency_gyro, latency_accel, latency;
	__le16 raw_wm;
	int ret;

	/* compute sensors latency, depending on sensor watermark and odr */
	wm_gyro = inv_icm45600_wm_truncate(st->fifo.watermark.gyro, packet_size,
					   st->fifo.period);
	wm_accel = inv_icm45600_wm_truncate(st->fifo.watermark.accel, packet_size,
					    st->fifo.period);
	/* use us for odr to avoid overflow using 32 bits values */
	period_gyro = inv_icm45600_odr_to_period(st->conf.gyro.odr) / 1000UL;
	period_accel = inv_icm45600_odr_to_period(st->conf.accel.odr) / 1000UL;
	latency_gyro = period_gyro * wm_gyro;
	latency_accel = period_accel * wm_accel;

	/* 0 value for watermark means that the sensor is turned off */
	if (wm_gyro == 0 && wm_accel == 0)
		return 0;

	if (latency_gyro == 0) {
		watermark = wm_accel;
		st->fifo.watermark.eff_accel = wm_accel;
	} else if (latency_accel == 0) {
		watermark = wm_gyro;
		st->fifo.watermark.eff_gyro = wm_gyro;
	} else {
		/* compute the smallest latency that is a multiple of both */
		if (latency_gyro <= latency_accel)
			latency = latency_gyro - (latency_accel % latency_gyro);
		else
			latency = latency_accel - (latency_gyro % latency_accel);
		/* use the shortest period */
		if (period_gyro <= period_accel)
			period = period_gyro;
		else
			period = period_accel;
		/* all this works because periods are multiple of each others */
		watermark = latency / period;
		if (watermark < 1)
			watermark = 1;
		/* update effective watermark */
		st->fifo.watermark.eff_gyro = latency / period_gyro;
		if (st->fifo.watermark.eff_gyro < 1)
			st->fifo.watermark.eff_gyro = 1;
		st->fifo.watermark.eff_accel = latency / period_accel;
		if (st->fifo.watermark.eff_accel < 1)
			st->fifo.watermark.eff_accel = 1;
	}

	raw_wm = INV_ICM45600_FIFO_WATERMARK_VAL(watermark);
	memcpy(st->buffer, &raw_wm, sizeof(raw_wm));
	ret = regmap_bulk_write(st->map, INV_ICM45600_REG_FIFO_WATERMARK,
				st->buffer, sizeof(raw_wm));
	if (ret)
		return ret;

	return 0;
}

static int inv_icm45600_buffer_preenable(struct iio_dev *indio_dev)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	struct device *dev = regmap_get_device(st->map);
	struct inv_icm45600_sensor_state *sensor_st = iio_priv(indio_dev);
	struct inv_sensors_timestamp *ts = &sensor_st->ts;

	pm_runtime_get_sync(dev);

	guard(mutex)(&st->lock);
	inv_sensors_timestamp_reset(ts);

	return 0;
}

/*
 * update_scan_mode callback is turning sensors on and setting data FIFO enable
 * bits.
 */
static int inv_icm45600_buffer_postenable(struct iio_dev *indio_dev)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	int ret;

	guard(mutex)(&st->lock);

	/* exit if FIFO is already on */
	if (st->fifo.on) {
		ret = 0;
		goto out_on;
	}

	/* flush FIFO data */
	ret = regmap_set_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG2,
			   INV_ICM45600_REG_FIFO_CONFIG2_FIFO_FLUSH);
	if (ret)
		return ret;

	/* set FIFO threshold and full interrupt */
	ret = regmap_set_bits(st->map, INV_ICM45600_REG_INT1_CONFIG0,
			      INV_ICM45600_INT1_CONFIG0_FIFO_THS_EN |
			      INV_ICM45600_INT1_CONFIG0_FIFO_FULL_EN);
	if (ret)
		return ret;

	/* set FIFO in streaming mode */
	ret = regmap_update_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG0,
				 INV_ICM45600_FIFO_CONFIG0_MODE_MASK,
				 INV_ICM45600_FIFO_CONFIG0_MODE_STREAM);
	if (ret)
		return ret;

	/* enable writing sensor data to FIFO */
	ret = regmap_set_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG3,
			      INV_ICM45600_FIFO_CONFIG3_IF_EN);
	if (ret)
		return ret;

out_on:
	/* increase FIFO on counter */
	st->fifo.on++;
	return ret;
}

static int inv_icm45600_buffer_predisable(struct iio_dev *indio_dev)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	int ret;

	guard(mutex)(&st->lock);

	/* exit if there are several sensors using the FIFO */
	if (st->fifo.on > 1) {
		ret = 0;
		goto out_off;
	}

	/* disable writing sensor data to FIFO */
	ret = regmap_clear_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG3,
				INV_ICM45600_FIFO_CONFIG3_IF_EN);
	if (ret)
		return ret;

	/* set FIFO in bypass mode */
	ret = regmap_update_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG0,
				 INV_ICM45600_FIFO_CONFIG0_MODE_MASK,
				 INV_ICM45600_FIFO_CONFIG0_MODE_BYPASS);
	if (ret)
		return ret;

	/* disable FIFO threshold and full interrupt */
	ret = regmap_clear_bits(st->map, INV_ICM45600_REG_INT1_CONFIG0,
				INV_ICM45600_INT1_CONFIG0_FIFO_THS_EN |
				INV_ICM45600_INT1_CONFIG0_FIFO_FULL_EN);
	if (ret)
		return ret;

	/* flush FIFO data */
	ret = regmap_set_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG2,
			   INV_ICM45600_REG_FIFO_CONFIG2_FIFO_FLUSH);
	if (ret)
		return ret;

out_off:
	/* decrease FIFO on counter */
	st->fifo.on--;
	return ret;
}

static int inv_icm45600_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct inv_icm45600_state *st = iio_device_get_drvdata(indio_dev);
	struct device *dev = regmap_get_device(st->map);
	unsigned int sensor;
	unsigned int *watermark;
	struct inv_icm45600_sensor_conf conf = INV_ICM45600_SENSOR_CONF_INIT;
	unsigned int sleep_temp = 0;
	unsigned int sleep_sensor = 0;
	unsigned int sleep;
	int ret;

	if (indio_dev == st->indio_gyro) {
		sensor = INV_ICM45600_SENSOR_GYRO;
		watermark = &st->fifo.watermark.gyro;
	} else if (indio_dev == st->indio_accel) {
		sensor = INV_ICM45600_SENSOR_ACCEL;
		watermark = &st->fifo.watermark.accel;
	} else {
		return -EINVAL;
	}

	scoped_guard(mutex, &st->lock) {
		ret = inv_icm45600_buffer_set_fifo_en(st, st->fifo.en & ~sensor);
		if (ret)
			break;

		*watermark = 0;
		ret = inv_icm45600_buffer_update_watermark(st);
		if (ret)
			break;

		conf.mode = INV_ICM45600_SENSOR_MODE_OFF;
		if (sensor == INV_ICM45600_SENSOR_GYRO)
			ret = inv_icm45600_set_gyro_conf(st, &conf, &sleep_sensor);
		else
			ret = inv_icm45600_set_accel_conf(st, &conf, &sleep_sensor);
	}
	/* sleep maximum required time */
	sleep = max(sleep_sensor, sleep_temp);
	if (sleep)
		msleep(sleep);

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

const struct iio_buffer_setup_ops inv_icm45600_buffer_ops = {
	.preenable = inv_icm45600_buffer_preenable,
	.postenable = inv_icm45600_buffer_postenable,
	.predisable = inv_icm45600_buffer_predisable,
	.postdisable = inv_icm45600_buffer_postdisable,
};

int inv_icm45600_buffer_fifo_read(struct inv_icm45600_state *st,
				  unsigned int max)
{
	const ssize_t packet_size = INV_ICM45600_FIFO_2SENSORS_PACKET_SIZE;
	__le16 *raw_fifo_count;
	size_t fifo_nb, i;
	ssize_t size;
	const void *accel, *gyro, *timestamp;
	const int8_t *temp;
	unsigned int odr;
	int ret;

	/* reset all samples counters */
	st->fifo.count = 0;
	st->fifo.nb.gyro = 0;
	st->fifo.nb.accel = 0;
	st->fifo.nb.total = 0;

	/* read FIFO count value */
	raw_fifo_count = (__le16 *)st->buffer;
	ret = regmap_bulk_read(st->map, INV_ICM45600_REG_FIFO_COUNT,
			       raw_fifo_count, sizeof(*raw_fifo_count));
	if (ret)
		return ret;
	fifo_nb = le16_to_cpup(raw_fifo_count);

	/* check and limit number of samples if requested */
	if (fifo_nb == 0)
		return 0;
	if (max > 0 && fifo_nb > max)
		fifo_nb = max;

	/* Try to read all FIFO data in internal buffer */
	st->fifo.count = fifo_nb * packet_size;
	ret = regmap_noinc_read(st->map, INV_ICM45600_REG_FIFO_DATA,
				st->fifo.data, st->fifo.count);
	if (ret == -ENOTSUPP || ret == -EFBIG) {
		/* Read full fifo is not supported, read samples one by one */
		ret = 0;
		for (i = 0; i < st->fifo.count && ret == 0; i += packet_size)
			ret = regmap_noinc_read(st->map, INV_ICM45600_REG_FIFO_DATA,
							&st->fifo.data[i], packet_size);
	}
	if (ret)
		return ret;

	for (i = 0; i < st->fifo.count; i += size) {
		size = inv_icm45600_fifo_decode_packet(&st->fifo.data[i],
			&accel, &gyro, &temp, &timestamp, &odr);
		if (size <= 0)
			break;
		if (gyro != NULL && inv_icm45600_fifo_is_data_valid(gyro))
			st->fifo.nb.gyro++;
		if (accel != NULL && inv_icm45600_fifo_is_data_valid(accel))
			st->fifo.nb.accel++;
		st->fifo.nb.total++;
	}

	return 0;
}

int inv_icm45600_buffer_fifo_parse(struct inv_icm45600_state *st)
{
	struct inv_icm45600_sensor_state *gyro_st = iio_priv(st->indio_gyro);
	struct inv_icm45600_sensor_state *accel_st = iio_priv(st->indio_accel);
	struct inv_sensors_timestamp *ts;
	int ret;

	if (st->fifo.nb.total == 0)
		return 0;

	/* handle gyroscope timestamp and FIFO data parsing */
	if (st->fifo.nb.gyro > 0) {
		ts = &gyro_st->ts;
		inv_sensors_timestamp_interrupt(ts, st->fifo.watermark.eff_gyro,
						st->timestamp.gyro);
		ret = inv_icm45600_gyro_parse_fifo(st->indio_gyro);
		if (ret)
			return ret;
	}

	/* handle accelerometer timestamp and FIFO data parsing */
	if (st->fifo.nb.accel > 0) {
		ts = &accel_st->ts;
		inv_sensors_timestamp_interrupt(ts, st->fifo.watermark.eff_accel,
						st->timestamp.accel);
		ret = inv_icm45600_accel_parse_fifo(st->indio_accel);
		if (ret)
			return ret;
	}

	return 0;
}

int inv_icm45600_buffer_hwfifo_flush(struct inv_icm45600_state *st,
				     unsigned int count)
{
	struct inv_icm45600_sensor_state *gyro_st = iio_priv(st->indio_gyro);
	struct inv_icm45600_sensor_state *accel_st = iio_priv(st->indio_accel);
	struct inv_sensors_timestamp *ts;
	int64_t gyro_ts, accel_ts;
	int ret;

	gyro_ts = iio_get_time_ns(st->indio_gyro);
	accel_ts = iio_get_time_ns(st->indio_accel);

	ret = inv_icm45600_buffer_fifo_read(st, count);
	if (ret)
		return ret;

	if (st->fifo.nb.total == 0)
		return 0;

	if (st->fifo.nb.gyro > 0) {
		ts = &gyro_st->ts;
		inv_sensors_timestamp_interrupt(ts, st->fifo.nb.gyro, gyro_ts);
		ret = inv_icm45600_gyro_parse_fifo(st->indio_gyro);
		if (ret)
			return ret;
	}

	if (st->fifo.nb.accel > 0) {
		ts = &accel_st->ts;
		inv_sensors_timestamp_interrupt(ts, st->fifo.nb.accel, accel_ts);
		ret = inv_icm45600_accel_parse_fifo(st->indio_accel);
		if (ret)
			return ret;
	}

	return 0;
}

int inv_icm45600_buffer_init(struct inv_icm45600_state *st)
{
	int ret;

	st->fifo.watermark.eff_gyro = 1;
	st->fifo.watermark.eff_accel = 1;

	/* Disable all FIFO EN bits. */
	ret = regmap_write(st->map, INV_ICM45600_REG_FIFO_CONFIG3, 0);
	if (ret)
		return ret;

	/* Disable FIFO and set depth */
	ret = regmap_write(st->map, INV_ICM45600_REG_FIFO_CONFIG0,
			   INV_ICM45600_FIFO_CONFIG0_MODE_BYPASS |
			   INV_ICM45600_FIFO_CONFIG0_FIFO_DEPTH_MAX);
	if (ret)
		return ret;

	/* enable only timestamp in fifo, disable compression */
	ret = regmap_write(st->map, INV_ICM45600_REG_FIFO_CONFIG4,
			   INV_ICM45600_FIFO_CONFIG4_TMST_FSYNC_EN);
	if (ret)
		return ret;

	/* Enable FIFO continuous watermark interrupt. */
	return regmap_set_bits(st->map, INV_ICM45600_REG_FIFO_CONFIG2,
			       INV_ICM45600_REG_FIFO_CONFIG2_WM_GT_TH);
}
