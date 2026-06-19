// SPDX-License-Identifier: GPL-2.0
/*
 * Altera Stratix 10 SoC FPGA hardware monitoring driver
 *
 * Copyright (c) 2026 Altera Corporation
 *
 * Authors:
 *	Nazim Amirul <muhammad.nazim.amirul.nazle.asmade@altera.com>
 *	Tze Yee Ng <tze.yee.ng@altera.com>
 */

#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/firmware/intel/stratix10-svc-client.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define HWMON_TIMEOUT			msecs_to_jiffies(SVC_HWMON_REQUEST_TIMEOUT_MS)
#define HWMON_RETRY_SLEEP_MS		1U
#define HWMON_ASYNC_MSG_RETRY		3U
#define STRATIX10_HWMON_MAXSENSORS	16
#define STRATIX10_HWMON_TEMPERATURE	"temperature"
#define STRATIX10_HWMON_VOLTAGE		"voltage"
#define STRATIX10_HWMON_CHANNEL_MASK	GENMASK(15, 0)
#define STRATIX10_HWMON_PAGE_SHIFT	16
#define STRATIX10_HWMON_ATTR_VISIBLE	0444
/* Temperature from SDM is signed Q8.8 millidegrees Celsius (8 fractional bits). */
#define STRATIX10_HWMON_TEMP_FRAC_BITS	8
#define STRATIX10_HWMON_TEMP_FRAC_DIV	BIT(STRATIX10_HWMON_TEMP_FRAC_BITS)
/* Voltage from SDM is unsigned Q16 (millivolts, 16 fractional bits). */
#define STRATIX10_HWMON_VOLT_FRAC_BITS	16
#define STRATIX10_HWMON_VOLT_FRAC_DIV	BIT(STRATIX10_HWMON_VOLT_FRAC_BITS)

#define ETEMP_INACTIVE			0x80000000U
#define ETEMP_TOO_OLD			0x80000001U
#define ETEMP_NOT_PRESENT		0x80000002U
#define ETEMP_TIMEOUT			0x80000003U
#define ETEMP_CORRUPT			0x80000004U
#define ETEMP_BUSY			0x80000005U
#define ETEMP_NOT_INITIALIZED		0x800000FFU

struct stratix10_hwmon_priv {
	struct stratix10_svc_chan *chan;
	struct stratix10_svc_client client;
	struct completion completion;
	struct mutex lock;	/* protect SVC calls */
	bool async;
	u32 temperature;
	u32 voltage;
	int temperature_channels;
	int voltage_channels;
	const char *temp_chan_names[STRATIX10_HWMON_MAXSENSORS];
	const char *volt_chan_names[STRATIX10_HWMON_MAXSENSORS];
	u32 temp_chan[STRATIX10_HWMON_MAXSENSORS];
	u32 volt_chan[STRATIX10_HWMON_MAXSENSORS];
};

static umode_t stratix10_hwmon_is_visible(const void *dev,
					  enum hwmon_sensor_types type,
					 u32 attr, int chan)
{
	const struct stratix10_hwmon_priv *priv = dev;

	switch (type) {
	case hwmon_temp:
		if (chan < priv->temperature_channels)
			return STRATIX10_HWMON_ATTR_VISIBLE;
		return 0;
	case hwmon_in:
		if (chan < priv->voltage_channels)
			return STRATIX10_HWMON_ATTR_VISIBLE;
		return 0;
	default:
		return 0;
	}
}

static void stratix10_hwmon_readtemp_cb(struct stratix10_svc_client *client,
					struct stratix10_svc_cb_data *data)
{
	struct stratix10_hwmon_priv *priv = client->priv;

	if (data->status == BIT(SVC_STATUS_OK)) {
		priv->temperature = (u32)*(unsigned long *)data->kaddr1;
	} else if (data->kaddr1) {
		dev_err(client->dev, "%s failed with status 0x%x, value 0x%lx\n",
			__func__, data->status,
			*(unsigned long *)data->kaddr1);
	} else {
		dev_err(client->dev, "%s failed with status 0x%x\n",
			__func__, data->status);
	}

	complete(&priv->completion);
}

static void stratix10_hwmon_readvolt_cb(struct stratix10_svc_client *client,
					struct stratix10_svc_cb_data *data)
{
	struct stratix10_hwmon_priv *priv = client->priv;

	if (data->status == BIT(SVC_STATUS_OK)) {
		priv->voltage = (u32)*(unsigned long *)data->kaddr1;
	} else if (data->kaddr1) {
		dev_err(client->dev, "%s failed with status 0x%x, value 0x%lx\n",
			__func__, data->status,
			*(unsigned long *)data->kaddr1);
	} else {
		dev_err(client->dev, "%s failed with status 0x%x\n",
			__func__, data->status);
	}

	complete(&priv->completion);
}

static void stratix10_hwmon_async_callback(void *ptr)
{
	if (ptr)
		complete(ptr);
}

static int stratix10_hwmon_parse_temp(long *val, u32 temperature)
{
	switch (temperature) {
	case ETEMP_INACTIVE:
	case ETEMP_NOT_PRESENT:
	case ETEMP_CORRUPT:
	case ETEMP_NOT_INITIALIZED:
		return -EOPNOTSUPP;
	case ETEMP_TIMEOUT:
	case ETEMP_BUSY:
	case ETEMP_TOO_OLD:
		return -EAGAIN;
	default:
		/* Convert Q8.8 millidegrees Celsius to millidegrees for hwmon. */
		*val = (long)(s32)temperature / STRATIX10_HWMON_TEMP_FRAC_DIV;
		return 0;
	}
}

static int stratix10_hwmon_encode_temp_arg(u32 reg, u64 *arg)
{
	u32 page = (reg >> STRATIX10_HWMON_PAGE_SHIFT) & STRATIX10_HWMON_CHANNEL_MASK;
	u32 channel = reg & STRATIX10_HWMON_CHANNEL_MASK;

	if (channel >= STRATIX10_HWMON_MAXSENSORS)
		return -EINVAL;

	*arg = (1ULL << channel) | ((u64)page << STRATIX10_HWMON_PAGE_SHIFT);
	return 0;
}

static int stratix10_hwmon_encode_volt_arg(u32 reg, u64 *arg)
{
	u32 channel = reg & STRATIX10_HWMON_CHANNEL_MASK;

	if (channel >= STRATIX10_HWMON_MAXSENSORS)
		return -EINVAL;

	*arg = 1ULL << channel;
	return 0;
}

static int stratix10_hwmon_async_read(struct device *dev,
				      enum hwmon_sensor_types type,
				     struct stratix10_svc_client_msg *msg)
{
	struct stratix10_hwmon_priv *priv = dev_get_drvdata(dev);
	struct stratix10_svc_cb_data data = {};
	struct completion completion;
	unsigned long wait_ret;
	void *handle = NULL;
	int status, index, ret;

	init_completion(&completion);

	for (index = 0; index < HWMON_ASYNC_MSG_RETRY; index++) {
		status = stratix10_svc_async_send(priv->chan, msg, &handle,
						  stratix10_hwmon_async_callback,
						  &completion);
		if (status == 0)
			break;
		dev_warn(dev, "Failed to send async message\n");
		msleep(HWMON_RETRY_SLEEP_MS);
	}

	if (status && !handle)
		return status;

	wait_ret = wait_for_completion_io_timeout(&completion, HWMON_TIMEOUT);
	if (wait_ret > 0)
		dev_dbg(dev, "Received async interrupt\n");
	else if (wait_ret == 0)
		dev_dbg(dev, "Timeout occurred, trying to poll the response\n");

	ret = -ETIMEDOUT;
	for (index = 0; index < HWMON_ASYNC_MSG_RETRY; index++) {
		status = stratix10_svc_async_poll(priv->chan, handle, &data);
		if (status == -EAGAIN) {
			dev_dbg(dev, "Async message is still in progress\n");
		} else if (status < 0) {
			dev_alert(dev, "Failed to poll async message: %d\n", status);
			ret = status;
			break;
		} else if (status == 0) {
			ret = 0;
			break;
		}
		msleep(HWMON_RETRY_SLEEP_MS);
	}

	if (ret) {
		dev_err(dev, "Failed to get async response\n");
		goto done;
	}

	if (data.status) {
		dev_err(dev, "%s returned 0x%x from SDM\n", __func__,
			data.status);
		ret = -EFAULT;
		goto done;
	}

	if (type == hwmon_temp)
		priv->temperature = (u32)*(unsigned long *)data.kaddr1;
	else
		priv->voltage = (u32)*(unsigned long *)data.kaddr1;

	ret = 0;

done:
	stratix10_svc_async_done(priv->chan, handle);
	return ret;
}

static int stratix10_hwmon_sync_read(struct device *dev,
				     enum hwmon_sensor_types type,
				    struct stratix10_svc_client_msg *msg)
{
	struct stratix10_hwmon_priv *priv = dev_get_drvdata(dev);
	int ret;

	reinit_completion(&priv->completion);

	if (type == hwmon_temp)
		priv->client.receive_cb = stratix10_hwmon_readtemp_cb;
	else
		priv->client.receive_cb = stratix10_hwmon_readvolt_cb;

	ret = stratix10_svc_send(priv->chan, msg);
	if (ret < 0)
		goto status_done;

	ret = wait_for_completion_interruptible_timeout(&priv->completion,
							HWMON_TIMEOUT);
	if (!ret) {
		dev_err(priv->client.dev, "timeout waiting for SMC call\n");
		ret = -ETIMEDOUT;
		goto status_done;
	}
	if (ret < 0) {
		dev_err(priv->client.dev, "error %d waiting for SMC call\n", ret);
		goto status_done;
	}

	ret = 0;

status_done:
	stratix10_svc_done(priv->chan);
	return ret;
}

static int stratix10_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
				u32 attr, int chan, long *val)
{
	struct stratix10_hwmon_priv *priv = dev_get_drvdata(dev);
	struct stratix10_svc_client_msg msg = {0};
	int ret;

	if (chan >= STRATIX10_HWMON_MAXSENSORS)
		return -EOPNOTSUPP;

	switch (type) {
	case hwmon_temp:
		ret = stratix10_hwmon_encode_temp_arg(priv->temp_chan[chan],
						      &msg.arg[0]);
		if (ret)
			return ret;
		msg.command = COMMAND_HWMON_READTEMP;
		break;
	case hwmon_in:
		ret = stratix10_hwmon_encode_volt_arg(priv->volt_chan[chan],
						      &msg.arg[0]);
		if (ret)
			return ret;
		msg.command = COMMAND_HWMON_READVOLT;
		break;
	default:
		return -EOPNOTSUPP;
	}

	guard(mutex)(&priv->lock);
	if (priv->async)
		ret = stratix10_hwmon_async_read(dev, type, &msg);
	else
		ret = stratix10_hwmon_sync_read(dev, type, &msg);
	if (ret)
		return ret;

	if (type == hwmon_temp)
		ret = stratix10_hwmon_parse_temp(val, priv->temperature);
	else
		/* Convert Q16 millivolts to millivolts for hwmon. */
		*val = (long)priv->voltage / STRATIX10_HWMON_VOLT_FRAC_DIV;
	return ret;
}

static int stratix10_hwmon_read_string(struct device *dev,
				       enum hwmon_sensor_types type, u32 attr,
				      int chan, const char **str)
{
	struct stratix10_hwmon_priv *priv = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_in:
		*str = priv->volt_chan_names[chan];
		return 0;
	case hwmon_temp:
		*str = priv->temp_chan_names[chan];
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops stratix10_hwmon_ops = {
	.is_visible = stratix10_hwmon_is_visible,
	.read = stratix10_hwmon_read,
	.read_string = stratix10_hwmon_read_string,
};

static const struct hwmon_channel_info *stratix10_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	NULL
};

static const struct hwmon_chip_info stratix10_hwmon_chip_info = {
	.ops = &stratix10_hwmon_ops,
	.info = stratix10_hwmon_info,
};

static int stratix10_hwmon_add_channel(struct device *dev, const char *type,
				       u32 val, const char *label,
				      struct stratix10_hwmon_priv *priv)
{
	if (!strcmp(type, STRATIX10_HWMON_TEMPERATURE)) {
		if (priv->temperature_channels >= STRATIX10_HWMON_MAXSENSORS) {
			dev_warn(dev, "Can't add temp node %s, too many channels\n",
				 label);
			return 0;
		}

		priv->temp_chan_names[priv->temperature_channels] = label;
		priv->temp_chan[priv->temperature_channels] = val;
		priv->temperature_channels++;
		return 0;
	}

	if (!strcmp(type, STRATIX10_HWMON_VOLTAGE)) {
		if (priv->voltage_channels >= STRATIX10_HWMON_MAXSENSORS) {
			dev_warn(dev, "Can't add voltage node %s, too many channels\n",
				 label);
			return 0;
		}

		priv->volt_chan_names[priv->voltage_channels] = label;
		priv->volt_chan[priv->voltage_channels] = val;
		priv->voltage_channels++;
		return 0;
	}

	dev_warn(dev, "unsupported sensor type %s\n", type);
	return 0;
}

static int stratix10_hwmon_probe_child_from_dt(struct device *dev,
					       struct device_node *child,
					      struct stratix10_hwmon_priv *priv)
{
	struct device_node *grandchild;
	const char *label;
	u32 val;
	int ret;

	for_each_child_of_node(child, grandchild) {
		ret = of_property_read_u32(grandchild, "reg", &val);
		if (ret) {
			dev_err(dev, "missing reg property of %pOFn\n",
				grandchild);
			of_node_put(grandchild);
			return ret;
		}

		ret = of_property_read_string(grandchild, "label", &label);
		if (ret)
			label = grandchild->name;

		stratix10_hwmon_add_channel(dev, child->name, val, label, priv);
	}

	return 0;
}

static int stratix10_hwmon_probe_from_dt(struct device *dev,
					 struct stratix10_hwmon_priv *priv)
{
	struct device_node *child;
	int ret;

	if (!dev->of_node)
		return 0;

	for_each_child_of_node(dev->of_node, child) {
		ret = stratix10_hwmon_probe_child_from_dt(dev, child, priv);
		if (ret) {
			of_node_put(child);
			return ret;
		}
	}

	return 0;
}

static int stratix10_hwmon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct stratix10_hwmon_priv *priv;
	struct device *hwmon_dev;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client.dev = dev;
	priv->client.priv = priv;
	init_completion(&priv->completion);
	mutex_init(&priv->lock);

	ret = stratix10_hwmon_probe_from_dt(dev, priv);
	if (ret) {
		dev_err(dev, "Unable to probe from device tree\n");
		return ret;
	}

	if (!priv->temperature_channels && !priv->voltage_channels) {
		dev_err(dev, "no temperature or voltage channels in device tree\n");
		return -ENODEV;
	}

	priv->chan = stratix10_svc_request_channel_byname(&priv->client,
							  SVC_CLIENT_HWMON);
	if (IS_ERR(priv->chan)) {
		ret = PTR_ERR(priv->chan);
		if (ret == -EPROBE_DEFER)
			dev_dbg(dev, "service channel %s not ready, deferring probe\n",
				SVC_CLIENT_HWMON);
		else
			dev_err(dev, "couldn't get service channel %s: %d\n",
				SVC_CLIENT_HWMON, ret);
		return ret;
	}

	ret = stratix10_svc_add_async_client(priv->chan, false);
	switch (ret) {
	case 0:
		priv->async = true;
		break;
	case -EINVAL:
		dev_dbg(dev, "async operations not supported, using sync mode\n");
		priv->async = false;
		break;
	default:
		dev_err(dev, "failed to add async client: %d\n", ret);
		stratix10_svc_free_channel(priv->chan);
		return ret;
	}

	dev_info(dev, "Initialized %d temperature and %d voltage channels\n",
		 priv->temperature_channels, priv->voltage_channels);

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "stratix10_hwmon",
							 priv,
							 &stratix10_hwmon_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev)) {
		if (priv->async)
			stratix10_svc_remove_async_client(priv->chan);
		stratix10_svc_free_channel(priv->chan);
		return PTR_ERR(hwmon_dev);
	}

	platform_set_drvdata(pdev, priv);
	return 0;
}

static void stratix10_hwmon_remove(struct platform_device *pdev)
{
	struct stratix10_hwmon_priv *priv = platform_get_drvdata(pdev);

	if (priv->async)
		stratix10_svc_remove_async_client(priv->chan);
	stratix10_svc_free_channel(priv->chan);
}

static const struct of_device_id stratix10_hwmon_of_match[] = {
	{ .compatible = "altr,stratix10-hwmon" },
	{}
};
MODULE_DEVICE_TABLE(of, stratix10_hwmon_of_match);

static struct platform_driver stratix10_hwmon_driver = {
	.driver = {
		.name = "stratix10-hwmon",
		.of_match_table = stratix10_hwmon_of_match,
	},
	.probe = stratix10_hwmon_probe,
	.remove = stratix10_hwmon_remove,
};
module_platform_driver(stratix10_hwmon_driver);

MODULE_AUTHOR("Nazim Amirul <muhammad.nazim.amirul.nazle.asmade@altera.com>");
MODULE_AUTHOR("Tze Yee Ng <tze.yee.ng@altera.com>");
MODULE_DESCRIPTION("Altera Stratix 10 SoC FPGA hardware monitoring driver");
MODULE_LICENSE("GPL");
