// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Invensense, Inc.
 * Copyright (C) 2026 Axis Communications AB
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/delay.h>

#include <linux/iio/buffer.h>
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

void inv_icm42370_buffer_update_fifo_period(struct inv_icm42370_data *st)
{
	u32 period_accel;

	if (st->fifo.en & INV_ICM42370_SENSOR_ACCEL)
		period_accel = inv_icm42370_odr_to_period(st->conf.odr);
	else
		period_accel = U32_MAX;

	st->fifo.period = period_accel;
}

int inv_icm42370_buffer_set_fifo_en(struct inv_icm42370_data *st,
				    unsigned int fifo_en)
{
	u8 mask, val, regval;
	int ret;

	/* update only FIFO EN bits */
	mask = INV_ICM42370_FIFO_CONFIG5_ACCEL_EN;

	val = 0;
	if (fifo_en & INV_ICM42370_SENSOR_ACCEL)
		val |= INV_ICM42370_FIFO_CONFIG5_ACCEL_EN;

	ret = inv_icm42370_mreg_read(st->map, INV_ICM42370_MREG1,
				     INV_ICM42370_REG_FIFO_CONFIG5, &regval);
	if (ret)
		return ret;

	/* clear the mask bits and set the new values */
	regval &= ~mask;
	regval |= val;

	ret = inv_icm42370_mreg_write(st->map, INV_ICM42370_MREG1,
				      INV_ICM42370_REG_FIFO_CONFIG5, regval);
	if (ret)
		return ret;

	st->fifo.en = fifo_en;
	inv_icm42370_buffer_update_fifo_period(st);

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
 * @st:	driver internal state
 *
 * Returns 0 on success, a negative error code otherwise.
 *
 * FIFO watermark threshold is computed based on the required
 * watermark values set for accel sensor.
 */
int inv_icm42370_buffer_update_watermark(struct inv_icm42370_data *st)
{
	size_t packet_size, wm_size;
	unsigned int wm_accel, watermark;
	bool restore;
	__le16 raw_wm;
	int ret;

	packet_size = inv_icm42370_get_packet_size(st->fifo.en);

	/* compute sensors latency, depending on sensor watermark and odr */
	wm_accel =
		inv_icm42370_wm_truncate(st->fifo.watermark.accel, packet_size);

	/* 0 value for watermark means that the sensor is turned off */
	if (wm_accel == 0)
		return 0;

	watermark = wm_accel;
	st->fifo.watermark.eff_accel = wm_accel;

	/* compute watermark value in bytes */
	wm_size = watermark * packet_size;

	/* changing FIFO watermark requires to turn off watermark interrupt */
	ret = regmap_update_bits_check(
		st->map, INV_ICM42370_REG_INT_SOURCE0,
		INV_ICM42370_INT_SOURCE0_FIFO_THS_INT1_EN, 0, &restore);
	if (ret)
		return ret;

	raw_wm = INV_ICM42370_FIFO_WATERMARK_VAL(wm_size);
	memcpy(st->buffer, &raw_wm, sizeof(raw_wm));
	ret = regmap_bulk_write(st->map, INV_ICM42370_REG_FIFO_WATERMARK,
				st->buffer, sizeof(raw_wm));
	if (ret)
		return ret;

	/* restore watermark interrupt */
	if (restore) {
		ret = regmap_set_bits(
			st->map, INV_ICM42370_REG_INT_SOURCE0,
			INV_ICM42370_INT_SOURCE0_FIFO_THS_INT1_EN);
		if (ret)
			return ret;
	}

	return 0;
}

static int inv_icm42370_buffer_preenable(struct iio_dev *indio_dev)
{
	struct inv_icm42370_data *st = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(st->map);
	struct inv_icm42370_sensor_state *sensor_st = st->sensor_state;
	struct inv_sensors_timestamp *ts = &sensor_st->ts;

	pm_runtime_get_sync(dev);

	guard(mutex)
		(&st->lock);
	inv_sensors_timestamp_reset(ts);

	return 0;
}

/**
 * update_scan_mode callback - turn sensor on and set data FIFO enable bits
 * @indio_dev: pointer to the industrial io struct
 *
 * Return 0 on success, negative errno on error
 */
static int inv_icm42370_buffer_postenable(struct iio_dev *indio_dev)
{
	struct inv_icm42370_data *st = iio_priv(indio_dev);
	int ret;

	guard(mutex)
		(&st->lock);

	if (st->fifo.on) {
		st->fifo.on++;
		return 0;
	}

	ret = regmap_write(st->map, INV_ICM42370_REG_SIGNAL_PATH_RESET,
			   INV_ICM42370_SIGNAL_PATH_RESET_FIFO_FLUSH);
	if (ret)
		return ret;

	ret = regmap_write(st->map, INV_ICM42370_REG_FIFO_CONFIG1,
			   INV_ICM42370_FIFO_CONFIG_STREAM);
	if (ret)
		return ret;

	ret = regmap_bulk_read(st->map, INV_ICM42370_REG_FIFO_COUNT, st->buffer,
			       2);
	if (ret)
		return ret;

	ret = regmap_set_bits(st->map, INV_ICM42370_REG_INT_SOURCE0,
			      INV_ICM42370_INT_SOURCE0_FIFO_THS_INT1_EN);
	if (ret)
		return ret;

	st->fifo.on++;

	return 0;
}

static int inv_icm42370_buffer_predisable(struct iio_dev *indio_dev)
{
	struct inv_icm42370_data *st = iio_priv(indio_dev);
	int ret;

	guard(mutex)
		(&st->lock);

	if (st->fifo.on > 1) {
		st->fifo.on--;
		return 0;
	}

	/* set FIFO in bypass mode */
	ret = regmap_write(st->map, INV_ICM42370_REG_FIFO_CONFIG1,
			   INV_ICM42370_FIFO_CONFIG_BYPASS);
	if (ret)
		return ret;

	/* flush FIFO data */
	ret = regmap_write(st->map, INV_ICM42370_REG_SIGNAL_PATH_RESET,
			   INV_ICM42370_SIGNAL_PATH_RESET_FIFO_FLUSH);
	if (ret)
		return ret;

	/* disable FIFO threshold interrupt */
	ret = regmap_clear_bits(st->map, INV_ICM42370_REG_INT_SOURCE0,
				INV_ICM42370_INT_SOURCE0_FIFO_THS_INT1_EN);
	if (ret)
		return ret;

	st->fifo.on--;

	return 0;
}

static int inv_icm42370_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct inv_icm42370_data *st = iio_priv(indio_dev);
	struct inv_icm42370_sensor_state *sensor_st = st->sensor_state;
	struct inv_sensors_timestamp *ts = &sensor_st->ts;
	struct device *dev = regmap_get_device(st->map);
	unsigned int sensor;
	unsigned int *watermark;
	struct inv_icm42370_conf conf = INV_ICM42370_SENSOR_CONF_INIT;
	unsigned int sleep_temp = 0;
	unsigned int sleep_sensor = 0;
	unsigned int sleep;
	int ret;

	if (indio_dev == st->indio_accel) {
		sensor = INV_ICM42370_SENSOR_ACCEL;
		watermark = &st->fifo.watermark.accel;
	} else {
		return -EINVAL;
	}

	mutex_lock(&st->lock);

	inv_sensors_timestamp_apply_odr(ts, 0, 0, 0);

	ret = inv_icm42370_buffer_set_fifo_en(st, st->fifo.en & ~sensor);
	if (ret)
		goto out_unlock;

	*watermark = 0;
	ret = inv_icm42370_buffer_update_watermark(st);
	if (ret)
		goto out_unlock;

	conf.mode = INV_ICM42370_SENSOR_MODE_OFF;
	ret = inv_icm42370_set_accel_conf(st, &conf, &sleep_sensor);
	if (ret)
		goto out_unlock;

out_unlock:
	mutex_unlock(&st->lock);

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

static int inv_icm42370_fifo_read_data(struct inv_icm42370_data *st,
				       size_t count)
{
	return regmap_noinc_read(st->map, INV_ICM42370_REG_FIFO_DATA,
				 st->fifo.data, count);
}

int inv_icm42370_buffer_fifo_read(struct inv_icm42370_data *st,
				  unsigned int max)
{
	size_t max_count;
	__be16 *raw_fifo_count;
	ssize_t i, size;
	const void *accel, *timestamp;
	const s8 *temp;
	unsigned int odr;
	int ret;

	/* reset all samples counters */
	st->fifo.count = 0;
	st->fifo.nb.accel = 0;
	st->fifo.nb.total = 0;

	/* compute maximum FIFO read size */
	if (max == 0)
		max_count = sizeof(st->fifo.data);
	else
		max_count = max * inv_icm42370_get_packet_size(st->fifo.en);

	/* read FIFO count value */
	raw_fifo_count = (__be16 *)st->buffer;
	ret = regmap_bulk_read(st->map, INV_ICM42370_REG_FIFO_COUNT,
			       raw_fifo_count, sizeof(*raw_fifo_count));
	if (ret)
		return ret;
	st->fifo.count = be16_to_cpup(raw_fifo_count);

	/* check and clamp FIFO count value */
	if (st->fifo.count == 0)
		return 0;
	if (st->fifo.count > max_count)
		st->fifo.count = max_count;

	/* read all FIFO data in internal buffer */
	ret = inv_icm42370_fifo_read_data(st, st->fifo.count);
	if (ret)
		return ret;

	/* compute number of samples for each sensor */
	for (i = 0; i < st->fifo.count; i += size) {
		size = inv_icm42370_fifo_decode_packet(
			&st->fifo.data[i], &accel, &temp, &timestamp, &odr);
		if (size <= 0)
			break;
		if (accel != NULL && inv_icm42370_fifo_is_data_valid(accel))
			st->fifo.nb.accel++;
		st->fifo.nb.total++;
	}

	return 0;
}

int inv_icm42370_buffer_fifo_parse(struct inv_icm42370_data *st)
{
	struct inv_icm42370_sensor_state *accel_st = st->sensor_state;
	struct inv_sensors_timestamp *ts;
	int ret;

	if (st->fifo.nb.total == 0)
		return 0;

	/* handle accelerometer timestamp and FIFO data parsing */
	if (st->fifo.nb.accel > 0) {
		ts = &accel_st->ts;
		inv_sensors_timestamp_interrupt(
			ts, st->fifo.watermark.eff_accel, st->timestamp);
		ret = inv_icm42370_accel_parse_fifo(st->indio_accel);
		if (ret)
			return ret;
	}

	return 0;
}

int inv_icm42370_buffer_hwfifo_flush(struct inv_icm42370_data *st,
				     unsigned int count)
{
	struct inv_icm42370_sensor_state *accel_st = st->sensor_state;
	struct inv_sensors_timestamp *ts;
	s64 accel_ts;
	int ret;

	accel_ts = iio_get_time_ns(st->indio_accel);

	ret = inv_icm42370_buffer_fifo_read(st, count);
	if (ret)
		return ret;

	if (st->fifo.nb.total == 0)
		return 0;

	if (st->fifo.nb.accel > 0) {
		ts = &accel_st->ts;
		inv_sensors_timestamp_interrupt(ts, st->fifo.nb.accel,
						accel_ts);
		ret = inv_icm42370_accel_parse_fifo(st->indio_accel);
		if (ret)
			return ret;
	}

	return 0;
}

int inv_icm42370_buffer_init(struct inv_icm42370_data *st)
{
	unsigned int val;
	u8 regval;
	int ret;

	st->fifo.watermark.eff_accel = 1;

	/* watermark should be set to a non-zero value before enabling interrupts */
	st->fifo.watermark.accel = 1;
	ret = inv_icm42370_buffer_update_watermark(st);
	if (ret)
		return ret;

	/*
	 * Default FIFO configuration (bits 6 to 5)
	 * - FIFO count in bytes
	 * - FIFO count in big endian
	 */
	val = INV_ICM42370_INTF_CONFIG0_FIFO_COUNT_ENDIAN;
	ret = regmap_update_bits(st->map, INV_ICM42370_REG_INTF_CONFIG0,
				 GENMASK(6, 5), val);
	if (ret)
		return ret;

	/*
	 * Enable FIFO partial read interrupt.
	 * Disable all FIFO EN bits.
	 */
	ret = inv_icm42370_mreg_read(st->map, INV_ICM42370_MREG1,
				     INV_ICM42370_REG_FIFO_CONFIG5, &regval);
	if (ret)
		return ret;

	regval &= ~(GENMASK(6, 5) | GENMASK(3, 0));
	regval |= INV_ICM42370_FIFO_CONFIG5_WM_GT_TH;
	regval |= INV_ICM42370_FIFO_CONFIG5_RESUME_PARTIAL_RD;

	return inv_icm42370_mreg_write(st->map, INV_ICM42370_MREG1,
				       INV_ICM42370_REG_FIFO_CONFIG5, regval);
}
