// SPDX-License-Identifier: GPL-2.0
/*
 * Charge state driver for ChromeOS Embedded Controller
 *
 * Copyright 2024 Google LLC
 *
 * This driver exports the low level control over charge chip connected to EC
 * which allows to manipulate the current used to charge the battery, and also
 * manipulate the current input to the whole system.
 * This driver also registers that charge chip as a thermal cooling device.
 */

#include <linux/of.h>
#include <linux/platform_data/cros_ec_commands.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

#define DRV_NAME "cros-ec-charge-state"
#define CHARGE_TYPE_CHARGE "charge"
#define CHARGE_TYPE_INPUT "input"

struct cros_ec_charge_state_data {
	struct cros_ec_device *ec_dev;
	struct device *dev;
	enum charge_state_params charge_type;
	uint32_t min_milliamp;
	uint32_t max_milliamp;
};

static int
cros_ec_charge_state_get_current_limit(struct cros_ec_device *ec_dev,
				       enum charge_state_params charge_type,
				       uint32_t *limit)
{
	struct ec_params_charge_state param;
	struct ec_response_charge_state state;
	int ret;

	param.cmd = CHARGE_STATE_CMD_GET_PARAM;
	param.get_param.param = charge_type;
	ret = cros_ec_cmd(ec_dev, 0, EC_CMD_CHARGE_STATE, &param, sizeof(param),
			  &state, sizeof(state));
	if (ret < 0)
		return ret;

	*limit = cpu_to_le32(state.get_param.value);
	return 0;
}

static int
cros_ec_charge_state_set_current_limit(struct cros_ec_device *ec_dev,
				       enum charge_state_params charge_type,
				       uint32_t limit)
{
	struct ec_params_charge_state param;
	int ret;

	param.cmd = CHARGE_STATE_CMD_SET_PARAM;
	param.set_param.param = charge_type;
	param.set_param.value = cpu_to_le32(limit);
	ret = cros_ec_cmd(ec_dev, 0, EC_CMD_CHARGE_STATE, &param, sizeof(param),
			  NULL, 0);
	return (ret < 0) ? ret : 0;
}

static int
cros_ec_charge_state_get_max_state(struct thermal_cooling_device *cdev,
				   unsigned long *state)
{
	struct cros_ec_charge_state_data *data = cdev->devdata;
	*state = data->max_milliamp;
	return 0;
}

static int
cros_ec_charge_state_get_cur_state(struct thermal_cooling_device *cdev,
				   unsigned long *state)
{
	struct cros_ec_charge_state_data *data = cdev->devdata;
	uint32_t limit;
	int ret;

	ret = cros_ec_charge_state_get_current_limit(data->ec_dev,
						     data->charge_type, &limit);
	if (ret) {
		dev_err(data->dev, "failed to get current state: %d", ret);
		return ret;
	}

	*state = data->max_milliamp - limit;
	return 0;
}

static int
cros_ec_charge_state_set_cur_state(struct thermal_cooling_device *cdev,
				   unsigned long state)
{
	struct cros_ec_charge_state_data *data = cdev->devdata;
	uint32_t limit = data->max_milliamp - state;

	if (limit < data->min_milliamp) {
		dev_warn(
			data->dev,
			"failed to set current %u lower than minimum %d; set to minimum",
			limit, data->min_milliamp);
		limit = data->min_milliamp;
	}

	state = data->max_milliamp - limit;
	return cros_ec_charge_state_set_current_limit(
		data->ec_dev, data->charge_type, (uint32_t)state);
}

static const struct thermal_cooling_device_ops
	cros_ec_charge_state_cooling_device_ops = {
		.get_max_state = cros_ec_charge_state_get_max_state,
		.get_cur_state = cros_ec_charge_state_get_cur_state,
		.set_cur_state = cros_ec_charge_state_set_cur_state,
	};

static int
cros_ec_charge_state_register_charge_chip(struct device *dev,
					  struct device_node *node,
					  struct cros_ec_device *cros_ec)
{
	struct cros_ec_charge_state_data *data;
	struct thermal_cooling_device *cdev;
	const char *type_val = NULL;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	ret = of_property_read_string(node, "type", &type_val);
	if (ret) {
		dev_err(dev, "failed to get charge type: %d", ret);
		return ret;
	}

	if (!strcmp(type_val, CHARGE_TYPE_CHARGE)) {
		data->charge_type = CS_PARAM_CHG_CURRENT;
	} else if (!strcmp(type_val, CHARGE_TYPE_INPUT)) {
		data->charge_type = CS_PARAM_CHG_INPUT_CURRENT;
	} else {
		dev_err(dev, "unknown charge type: %s", type_val);
		return -1;
	}

	ret = of_property_read_u32(node, "min-milliamp", &data->min_milliamp);
	if (ret) {
		dev_err(dev, "failed to get min-milliamp data: %d", ret);
		return ret;
	}

	ret = of_property_read_u32(node, "max-milliamp", &data->max_milliamp);
	if (ret) {
		dev_err(dev, "failed to get max-milliamp data: %d", ret);
		return ret;
	}

	data->ec_dev = cros_ec;
	data->dev = dev;

	cdev = devm_thermal_of_cooling_device_register(
		dev, node, node->name, data,
		&cros_ec_charge_state_cooling_device_ops);
	if (IS_ERR_VALUE(cdev)) {
		dev_err(dev,
			"failed to register charge chip %s as cooling device: %pe",
			node->name, cdev);
		return PTR_ERR(cdev);
	}

	return 0;
}

static int cros_ec_charge_state_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cros_ec_dev *ec_dev = dev_get_drvdata(dev->parent);
	struct cros_ec_device *cros_ec = ec_dev->ec_dev;
	struct device_node *child;

	for_each_available_child_of_node(cros_ec->dev->of_node, child) {
		if (!of_device_is_compatible(child,
					     "google,cros-ec-charge-state"))
			continue;
		if (cros_ec_charge_state_register_charge_chip(dev, child,
							      cros_ec))
			continue;
	}

	return 0;
}

static const struct platform_device_id cros_ec_charge_state_id[] = {
	{ DRV_NAME,  0 },
	{}
};

static struct platform_driver cros_ec_chargedev_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.probe = cros_ec_charge_state_probe,
};

module_platform_driver(cros_ec_chargedev_driver);

MODULE_DEVICE_TABLE(platform, cros_ec_charge_state_id);
MODULE_DESCRIPTION("ChromeOS EC Charge State Driver");
MODULE_AUTHOR("Sung-Chi, Li <lschyi@chromium.org>");
MODULE_LICENSE("GPL");
