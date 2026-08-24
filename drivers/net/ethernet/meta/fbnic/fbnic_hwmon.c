// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include <linux/hwmon.h>
#include <linux/jiffies.h>

#include "fbnic.h"
#include "fbnic_mac.h"

static umode_t fbnic_hwmon_is_visible(const void *drvdata,
				      enum hwmon_sensor_types type,
				      u32 attr, int channel)
{
	return 0444;
}

static int fbnic_hwmon_sensor_read(struct fbnic_dev *fbd, int id, long *val)
{
	struct fbnic_hwmon_cache *cache = &fbd->hwmon_cache;
	struct fbnic_fw_completion *fw_cmpl;
	int err = 0;
	s32 *cached;

	switch (id) {
	case FBNIC_SENSOR_TEMP:
		cached = &cache->temp_mdeg;
		break;
	case FBNIC_SENSOR_VOLTAGE:
		cached = &cache->volt_mv;
		break;
	default:
		return -EINVAL;
	}

	if (*cached != FBNIC_SENSOR_NO_DATA &&
	    time_is_after_eq_jiffies(cache->last_read)) {
		*val = *cached;
		return 0;
	}

	fw_cmpl = fbnic_fw_alloc_cmpl(FBNIC_TLV_MSG_ID_TSENE_READ_RESP);
	if (!fw_cmpl)
		return -ENOMEM;

	err = fbnic_fw_xmit_tsene_read_msg(fbd, fw_cmpl);
	if (err) {
		dev_err(fbd->dev,
			"Failed to transmit TSENE read msg, err %d\n",
			err);
		goto exit_free;
	}

	if (!wait_for_completion_timeout(&fw_cmpl->done, 10 * HZ)) {
		dev_err(fbd->dev, "Timed out waiting for TSENE read\n");
		err = -ETIMEDOUT;
		goto exit_cleanup;
	}

	/* Handle error returned by firmware */
	if (fw_cmpl->result) {
		err = fw_cmpl->result;
		dev_err(fbd->dev, "%s: Firmware returned error %d\n",
			__func__, err);
		goto exit_cleanup;
	}

	/* FW returns both readings in one response, cache both. */
	cache->temp_mdeg = fw_cmpl->u.tsene.millidegrees;
	cache->volt_mv = fw_cmpl->u.tsene.millivolts;
	cache->last_read = jiffies;

	*val = *cached;
exit_cleanup:
	fbnic_mbx_clear_cmpl(fbd, fw_cmpl);
exit_free:
	fbnic_fw_put_cmpl(fw_cmpl);

	return err;
}

static int fbnic_hwmon_read_threshold(long thr, long *val)
{
	if (thr == FBNIC_SENSOR_NO_DATA)
		return -ENODATA;

	*val = thr;
	return 0;
}

static int fbnic_hwmon_read_alarm(long a, long b, long *val)
{
	if (a == FBNIC_SENSOR_NO_DATA || b == FBNIC_SENSOR_NO_DATA)
		return -ENODATA;

	*val = a >= b;
	return 0;
}

static int fbnic_hwmon_temp_read(struct fbnic_dev *fbd, u32 attr, long *val)
{
	int err;

	switch (attr) {
	case hwmon_temp_min:
		return fbnic_hwmon_read_threshold(fbd->fw_cap.temp.min, val);
	case hwmon_temp_max:
		return fbnic_hwmon_read_threshold(fbd->fw_cap.temp.max, val);
	case hwmon_temp_crit:
		return fbnic_hwmon_read_threshold(fbd->fw_cap.temp.crit, val);
	case hwmon_temp_input:
	case hwmon_temp_min_alarm:
	case hwmon_temp_max_alarm:
	case hwmon_temp_crit_alarm:
		break;
	default:
		return -EOPNOTSUPP;
	}

	err = fbnic_hwmon_sensor_read(fbd, FBNIC_SENSOR_TEMP, val);
	if (err)
		return err;

	switch (attr) {
	case hwmon_temp_input:
		return 0;
	case hwmon_temp_min_alarm:
		return fbnic_hwmon_read_alarm(fbd->fw_cap.temp.min, *val, val);
	case hwmon_temp_max_alarm:
		return fbnic_hwmon_read_alarm(*val, fbd->fw_cap.temp.max, val);
	case hwmon_temp_crit_alarm:
		return fbnic_hwmon_read_alarm(*val, fbd->fw_cap.temp.crit, val);
	}

	return -EOPNOTSUPP;
}

static int fbnic_hwmon_in_read(struct fbnic_dev *fbd, u32 attr, long *val)
{
	int err;

	switch (attr) {
	case hwmon_in_min:
		return fbnic_hwmon_read_threshold(fbd->fw_cap.volt.min, val);
	case hwmon_in_max:
		return fbnic_hwmon_read_threshold(fbd->fw_cap.volt.max, val);
	case hwmon_in_input:
	case hwmon_in_min_alarm:
	case hwmon_in_max_alarm:
		break;
	default:
		return -EOPNOTSUPP;
	}

	err = fbnic_hwmon_sensor_read(fbd, FBNIC_SENSOR_VOLTAGE, val);
	if (err)
		return err;

	switch (attr) {
	case hwmon_in_input:
		return 0;
	case hwmon_in_min_alarm:
		return fbnic_hwmon_read_alarm(fbd->fw_cap.volt.min, *val, val);
	case hwmon_in_max_alarm:
		return fbnic_hwmon_read_alarm(*val, fbd->fw_cap.volt.max, val);
	}

	return -EOPNOTSUPP;
}

static int fbnic_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long *val)
{
	struct fbnic_dev *fbd = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		return fbnic_hwmon_temp_read(fbd, attr, val);
	case hwmon_in:
		return fbnic_hwmon_in_read(fbd, attr, val);
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops fbnic_hwmon_ops = {
	.is_visible = fbnic_hwmon_is_visible,
	.read = fbnic_hwmon_read,
};

static const struct hwmon_channel_info *fbnic_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT |
			   HWMON_T_MIN | HWMON_T_MIN_ALARM |
			   HWMON_T_MAX | HWMON_T_MAX_ALARM |
			   HWMON_T_CRIT | HWMON_T_CRIT_ALARM),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT |
			   HWMON_I_MIN | HWMON_I_MIN_ALARM |
			   HWMON_I_MAX | HWMON_I_MAX_ALARM),
	NULL
};

static const struct hwmon_chip_info fbnic_chip_info = {
	.ops = &fbnic_hwmon_ops,
	.info = fbnic_hwmon_info,
};

void fbnic_hwmon_register(struct fbnic_dev *fbd)
{
	if (!IS_REACHABLE(CONFIG_HWMON))
		return;

	/* Seed cache with sentinel so the first read always refreshes. */
	fbd->hwmon_cache.temp_mdeg = FBNIC_SENSOR_NO_DATA;
	fbd->hwmon_cache.volt_mv = FBNIC_SENSOR_NO_DATA;

	fbd->hwmon = hwmon_device_register_with_info(fbd->dev, "fbnic",
						     fbd, &fbnic_chip_info,
						     NULL);
	if (IS_ERR(fbd->hwmon)) {
		dev_notice(fbd->dev,
			   "Failed to register hwmon device %pe\n",
			   fbd->hwmon);
		fbd->hwmon = NULL;
	}
}

void fbnic_hwmon_unregister(struct fbnic_dev *fbd)
{
	if (!IS_REACHABLE(CONFIG_HWMON) || !fbd->hwmon)
		return;

	hwmon_device_unregister(fbd->hwmon);
	fbd->hwmon = NULL;
}
