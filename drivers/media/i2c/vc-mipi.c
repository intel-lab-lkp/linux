// SPDX-License-Identifier: GPL-2.0
/*
 * Vision Components MIPI Module Controller
 *
 * Copyright 2021 Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 */

#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

#define VC_MIPI_REG_RESET			0x0100
#define VC_MIPI_REG_RESET_POWER_DOWN		BIT(1)
#define VC_MIPI_REG_RESET_RESET			BIT(0)

#define VC_MIPI_REG_STATUS			0x0101
#define VC_MIPI_REG_STATUS_OFF			0x00
#define VC_MIPI_REG_STATUS_ERROR		0x01
#define VC_MIPI_REG_STATUS_ON			0x80

#define VC_MIPI_REG_MODE			0x0102
#define VC_MIPI_REG_MODE_STREAM_10BIT		0x00
#define VC_MIPI_REG_MODE_STREAM_8BIT		0x01
#define VC_MIPI_REG_MODE_EXTTRIG_10BIT		0x02
#define VC_MIPI_REG_MODE_EXTTRIG_8BIT		0x03
#define VC_MIPI_REG_MODE_TESTIMG_10BIT		0xF0
#define VC_MIPI_REG_MODE_TESTIMG_8BIT		0xF1

#define VC_MIPI_REG_IO				0x0103
#define VC_MIPI_REG_IO_XTRIG_ACTIVE_LOW		BIT(6)
#define VC_MIPI_REG_IO_FS_ACTIVE_LOW		BIT(5)
#define VC_MIPI_REG_IO_FLASH_ACTIVE_LOW		BIT(4)
#define VC_MIPI_REG_IO_XTRIG_ENABLE		BIT(3)
#define VC_MIPI_REG_IO_EXCHANGE_FLASH_TRIGGER	BIT(2)
#define VC_MIPI_REG_IO_FS_ON_TRIGGER_ENABLE	BIT(1)
#define VC_MIPI_REG_IO_FLASH_ENABLE		BIT(0)
#define VC_MIPI_REG_IO_DEFAULT			VC_MIPI_REG_IO_XTRIG_ENABLE

#define VC_MIPI_REG_MODULE_ADDR			0x0104
#define VC_MIPI_REG_MODULE_ADDR_DEFAULT		0x10

#define VC_MIPI_REG_SENSOR_ADDR			0x0105
#define VC_MIPI_REG_SENSOR_ADDR_DEFAULT		0x60

#define VC_MIPI_REG_OUTPUT_OVERRIDE		0x0106
#define VC_MIPI_REG_OUTPUT_OVERRIDE_TRIGGER_OVR	BIT(5)
#define VC_MIPI_REG_OUTPUT_OVERRIDE_FLASH_OVR	BIT(4)
#define VC_MIPI_REG_OUTPUT_OVERRIDE_TRIGGER_VAL	BIT(1)
#define VC_MIPI_REG_OUTPUT_OVERRIDE_FLASH_VAL	BIT(0)

#define VC_MIPI_REG_INPUT			0x0107
#define VC_MIPI_REG_INPUT_TRIGGER		BIT(1)
#define VC_MIPI_REG_INPUT_FLASH			BIT(0)

#define VC_MIPI_REG_EXTTRIG			0x0108
#define VC_MIPI_REG_EXTTRIG_DISABLE		0x00
#define VC_MIPI_REG_EXTTRIG_ENABLE		BIT(0)

#define VC_MIPI_REG_ROM				0x1000

struct vc_mipi_ctrl {
	struct device *dev;
	struct regmap *regmap;
	struct regulator *supply;
	struct clk_hw *clk_hw;
	struct i2c_adapter i2c_tunnel;
	bool enabled;
};

/* -----------------------------------------------------------------------------
 * Regulator
 */

static int vc_mipi_regulator_enable(struct regulator_dev *rdev)
{
	struct vc_mipi_ctrl *ctrl = rdev_get_drvdata(rdev);
	unsigned int val;
	int ret;

	ret = pm_runtime_resume_and_get(ctrl->dev);
	if (ret)
		return ret;

	ret = regmap_write(rdev->regmap, VC_MIPI_REG_RESET, 0);
	if (ret < 0)
		goto error;

	msleep(500);

	ret = regmap_read(rdev->regmap, VC_MIPI_REG_STATUS, &val);
	if (ret < 0)
		goto error;

	if (val != VC_MIPI_REG_STATUS_ON) {
		dev_err(&rdev->dev, "Sensor failed to initialize (0x%02x)\n",
			val);
		ret = -EIO;
		goto error;
	}

	ret = regmap_write(rdev->regmap, VC_MIPI_REG_IO,
			   (VC_MIPI_REG_IO_DEFAULT | VC_MIPI_REG_IO_FLASH_ENABLE));
	if (ret < 0) {
		dev_err(&rdev->dev, "Failed to enable flash: %d\n", ret);
		goto error;
	}

	ret = regmap_write(ctrl->regmap, VC_MIPI_REG_EXTTRIG,
			   VC_MIPI_REG_EXTTRIG_DISABLE);
	if (ret < 0) {
		dev_err(&rdev->dev,
			"Failed to set up external flash trigger: %d\n", ret);
		goto error;
	}

	ctrl->enabled = true;

	return 0;

error:
	pm_runtime_mark_last_busy(ctrl->dev);
	pm_runtime_put_autosuspend(ctrl->dev);
	return ret;
}

static int vc_mipi_regulator_disable(struct regulator_dev *rdev)
{
	struct vc_mipi_ctrl *ctrl = rdev_get_drvdata(rdev);
	int ret;

	ret = regmap_write(ctrl->regmap, VC_MIPI_REG_EXTTRIG,
			   VC_MIPI_REG_EXTTRIG_DISABLE);
	if (ret < 0) {
		dev_err(&rdev->dev,
			"Failed to set up external flash trigger: %d\n", ret);
		goto autosuspend;
	}

	ret = regmap_write(rdev->regmap, VC_MIPI_REG_IO,
			   (VC_MIPI_REG_IO_DEFAULT | VC_MIPI_REG_IO_FLASH_ENABLE));
	if (ret < 0) {
		dev_err(&rdev->dev, "Failed to disable flash: %d\n", ret);
		goto autosuspend;
	}

	ret = regmap_write(rdev->regmap, VC_MIPI_REG_RESET,
			   VC_MIPI_REG_RESET_POWER_DOWN |
			   VC_MIPI_REG_RESET_RESET);

autosuspend:
	pm_runtime_mark_last_busy(ctrl->dev);
	pm_runtime_put_autosuspend(ctrl->dev);

	ctrl->enabled = false;

	return ret;
}

static int vc_mipi_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct vc_mipi_ctrl *ctrl = rdev_get_drvdata(rdev);

	return ctrl->enabled;
}

static const struct regulator_ops vc_mipi_regulator_ops = {
	.enable = vc_mipi_regulator_enable,
	.disable = vc_mipi_regulator_disable,
	.is_enabled = vc_mipi_regulator_is_enabled,
};

static const struct regulator_desc vc_mipi_regulator = {
	.name = "vc-mipi",
	.ops = &vc_mipi_regulator_ops,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
};

static const struct regulator_init_data vc_mipi_regulator_init_data = {
	.constraints = {
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
};

static int vc_mipi_regulator_init(struct vc_mipi_ctrl *ctrl)
{
	struct regulator_config config = { };
	struct regulator_dev *rdev;

	config.dev = ctrl->dev;
	config.init_data = &vc_mipi_regulator_init_data;
	config.driver_data = ctrl;
	config.of_node = ctrl->dev->of_node;
	config.regmap = ctrl->regmap;

	rdev = devm_regulator_register(ctrl->dev, &vc_mipi_regulator, &config);
	if (IS_ERR(rdev))
		return PTR_ERR(rdev);

	return 0;
}

/* -----------------------------------------------------------------------------
 * Clock
 */

static int vc_mipi_clk_init(struct vc_mipi_ctrl *ctrl)
{
	char name[20];
	u32 freq;
	int ret;

	ret = of_property_read_u32(ctrl->dev->of_node, "clock-frequency",
				   &freq);
	if (ret < 0) {
		dev_err(ctrl->dev, "Failed to retrieve clock frequency: %d\n",
			ret);
		return ret;
	}

	/*
	 * As this is an I2C device, the device name will be in the form
	 * 'bus-addr', where bus is an integer and addr a 4 characters hex
	 * value. 20 bytes should be enough as there shouldn't be more than 100
	 * I2C buses.
	 */
	snprintf(name, sizeof(name), "vc-mipi-%s-clk", dev_name(ctrl->dev));
	ctrl->clk_hw = clk_hw_register_fixed_rate(ctrl->dev, name, NULL, 0,
						  freq);
	if (IS_ERR(ctrl->clk_hw))
		return PTR_ERR(ctrl->clk_hw);

	ret = devm_of_clk_add_hw_provider(ctrl->dev, of_clk_hw_simple_get,
					  ctrl->clk_hw);
	if (ret < 0) {
		clk_hw_unregister_fixed_rate(ctrl->clk_hw);
		return ret;
	}

	return 0;
}

static void vc_mipi_clk_cleanup(struct vc_mipi_ctrl *ctrl)
{
	clk_hw_unregister_fixed_rate(ctrl->clk_hw);
}

/* -----------------------------------------------------------------------------
 * Power management
 */

static int vc_mipi_power_on(struct device *dev)
{
	struct vc_mipi_ctrl *ctrl = dev_get_drvdata(dev);
	int ret;

	ret = regulator_enable(ctrl->supply);
	if (ret < 0) {
		dev_err(ctrl->dev, "Failed to enable vcc supply: %d\n", ret);
		return ret;
	}

	return 0;
}

static int vc_mipi_power_off(struct device *dev)
{
	struct vc_mipi_ctrl *ctrl = dev_get_drvdata(dev);

	regulator_disable(ctrl->supply);

	return 0;
}

static const struct dev_pm_ops vc_mipi_pm_ops = {
	SET_RUNTIME_PM_OPS(vc_mipi_power_off, vc_mipi_power_on, NULL)
};

/* -----------------------------------------------------------------------------
 * I2C Proxy
 */

static int vc_mipi_i2c_tunnel_xfer(struct i2c_adapter *i2c_adapter, struct i2c_msg *msgs, int num)
{
	struct i2c_client *i2c = i2c_get_adapdata(i2c_adapter);
	struct i2c_adapter *parent = i2c->adapter;
	int ret;

	i2c_lock_bus(parent, I2C_LOCK_SEGMENT);
	ret = __i2c_transfer(i2c->adapter, msgs, num);
	i2c_unlock_bus(parent, I2C_LOCK_SEGMENT);

	if (ret < 0)
		return ret;
	if (ret != num)
		return -EIO;
	return ret;
}

static u32 vc_mipi_i2c_tunnel_functionality(struct i2c_adapter *i2c_adapter)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm vc_mipi_i2c_tunnel_algorithm = {
	.master_xfer   = vc_mipi_i2c_tunnel_xfer,
	.functionality = vc_mipi_i2c_tunnel_functionality,
};

/* Controller firmware had a bug which prevented reading more than one byte
 * via I2C from the sensor. This was (according to the vendor) fixed somewhere
 * in 2026. Nonetheless we must support also modules running old Firmware.
 * Therefore set up the i2c quirks accordingly.
 *
 * NOTE: This is different from the "I2C reads with address increment return
 *       the first byte twice" bug, which affects the controller, not the
 *       sensor.
 */
static const struct i2c_adapter_quirks vc_mipi_i2c_tunnel_quirks = {
	.max_read_len = 1,
};

static struct device_node *vc_mipi_i2c_find_tunnel_bus(struct device *dev)
{
	struct device_node *child;

	for_each_child_of_node(dev->of_node, child) {
		u32 addr_cells, size_cells;

		if (of_property_read_u32(child, "#address-cells", &addr_cells))
			continue;
		if (of_property_read_u32(child, "#size-cells", &size_cells))
			continue;
		if (addr_cells != 1 || size_cells != 0)
			continue;

		return of_node_get(child);
	}

	return NULL;
}

static int vc_mipi_i2c_tunnel_add(struct i2c_client *i2c)
{
	struct vc_mipi_ctrl *ctrl = i2c_get_clientdata(i2c);
	struct i2c_adapter *i2c_adapter = &ctrl->i2c_tunnel;
	struct device_node *bus_node;

	bus_node = vc_mipi_i2c_find_tunnel_bus(ctrl->dev);
	if (!bus_node)
		return -EINVAL;

	strscpy(i2c_adapter->name, "vc-mipi-i2c", sizeof(i2c_adapter->name));
	i2c_adapter->owner       = THIS_MODULE;
	i2c_adapter->algo        = &vc_mipi_i2c_tunnel_algorithm;
	i2c_adapter->quirks      = &vc_mipi_i2c_tunnel_quirks;
	i2c_adapter->dev.parent  = ctrl->dev;
	i2c_adapter->dev.of_node = bus_node;

	i2c_set_adapdata(i2c_adapter, i2c);
	return i2c_add_adapter(&ctrl->i2c_tunnel);
}

static void vc_mipi_i2c_tunnel_del(struct i2c_client *i2c)
{
	struct vc_mipi_ctrl *ctrl = i2c_get_clientdata(i2c);

	i2c_del_adapter(&ctrl->i2c_tunnel);
}

/* -----------------------------------------------------------------------------
 * Probe & Remove
 */

/*
 * Due to a bug in the firmware, I2C reads with address increment return the
 * first byte twice. The dummy byte at the beginning of the ROM descriptor
 * works around the issue, at the cost of requiring unaligned accesses.
 */
struct vc_mipi_descriptor_rom {
	u8 dummy;
	u8 magic[12];
	u8 manufacturer[32];
	__le16 mipi_mid;
	u8 sensor_manufacturer[8];
	u8 sensor_model[16];
	__le16 module_id;
	__le16 module_rev;
} __packed;

static int vc_mipi_identify(struct vc_mipi_ctrl *ctrl)
{
	struct vc_mipi_descriptor_rom rom;
	unsigned int addr;
	int ret;

	ret = regmap_raw_read(ctrl->regmap, VC_MIPI_REG_ROM, &rom, sizeof(rom));
	if (ret < 0) {
		dev_err(ctrl->dev, "Failed to read ROM: %d\n", ret);
		return ret;
	}

	if (memcmp(&rom.magic, "mipi-module", sizeof(rom.magic))) {
		dev_err(ctrl->dev, "Invalid ROM magic value\n");
		print_hex_dump(KERN_INFO, "rom: ", DUMP_PREFIX_OFFSET, 16, 1,
			       &rom, sizeof(rom), true);
		return -EINVAL;
	}

	ret = regmap_read(ctrl->regmap, VC_MIPI_REG_SENSOR_ADDR, &addr);
	if (ret < 0) {
		dev_err(ctrl->dev, "Failed to read sensor address: %d\n", ret);
		return ret;
	}

	dev_info(ctrl->dev, "%.8s %.16s (%04x:%04x @0x%02x)\n",
		 rom.sensor_manufacturer, rom.sensor_model,
		 le16_to_cpu(rom.module_id), le16_to_cpu(rom.module_rev), addr);

	return 0;
}

static const struct regmap_config vc_mipi_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_NONE,
};

static int vc_mipi_i2c_probe(struct i2c_client *i2c)
{
	struct vc_mipi_ctrl *ctrl;
	int ret;

	ctrl = devm_kzalloc(&i2c->dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->dev = &i2c->dev;

	i2c_set_clientdata(i2c, ctrl);

	ctrl->supply = devm_regulator_get(ctrl->dev, "vcc");
	if (IS_ERR(ctrl->supply)) {
		ret = PTR_ERR(ctrl->supply);
		dev_err(ctrl->dev, "Failed to get vcc supply: %d\n", ret);
		return ret;
	}

	ctrl->regmap = devm_regmap_init_i2c(i2c, &vc_mipi_regmap_config);
	if (IS_ERR(ctrl->regmap)) {
		ret = PTR_ERR(ctrl->regmap);
		dev_err(ctrl->dev, "Failed to init regmap: %d\n", ret);
		return ret;
	}

	ret = vc_mipi_power_on(ctrl->dev);
	if (ret < 0)
		return ret;

	ret = vc_mipi_identify(ctrl);
	if (ret < 0)
		goto err_power;

	ret = vc_mipi_regulator_init(ctrl);
	if (ret < 0) {
		dev_err(ctrl->dev, "Failed to register regulator\n");
		goto err_power;
	}

	ret = vc_mipi_clk_init(ctrl);
	if (ret < 0) {
		dev_err(ctrl->dev, "Failed to register clock\n");
		goto err_power;
	}

	/* Enable runtime PM and turn off the device. */
	pm_runtime_set_active(ctrl->dev);
	pm_runtime_get_noresume(ctrl->dev);
	pm_runtime_enable(ctrl->dev);
	pm_runtime_set_autosuspend_delay(ctrl->dev, 1000);
	pm_runtime_use_autosuspend(ctrl->dev);
	pm_runtime_mark_last_busy(ctrl->dev);
	pm_runtime_put_autosuspend(ctrl->dev);

	/* As a last step create the proxied downstream I2C adapter */
	ret = vc_mipi_i2c_tunnel_add(i2c);
	if (ret < 0) {
		dev_err(ctrl->dev, "Failed to register i2c tunnel adapter: %d\n", ret);
		goto err_power;
	}

	return 0;

err_power:
	vc_mipi_power_off(ctrl->dev);
	return ret;
}

static void vc_mipi_i2c_remove(struct i2c_client *i2c)
{
	struct vc_mipi_ctrl *ctrl = i2c_get_clientdata(i2c);

	vc_mipi_i2c_tunnel_del(i2c);

	vc_mipi_clk_cleanup(ctrl);

	pm_runtime_disable(ctrl->dev);
	if (!pm_runtime_status_suspended(ctrl->dev))
		vc_mipi_power_off(ctrl->dev);
	pm_runtime_set_suspended(ctrl->dev);
}

static const struct of_device_id vc_mipi_dt_ids[] = {
	{ .compatible = "vision-components,mipi-module-controller" },
	{},
};
MODULE_DEVICE_TABLE(of, vc_mipi_dt_ids);

static struct i2c_driver vc_mipi_driver = {
	.driver = {
		.name = "vc-mipi",
		.of_match_table = vc_mipi_dt_ids,
		.pm = &vc_mipi_pm_ops,
	},
	.probe = vc_mipi_i2c_probe,
	.remove = vc_mipi_i2c_remove,
};

module_i2c_driver(vc_mipi_driver);

MODULE_AUTHOR("Laurent Pinchart <laurent.pinchart@ideasonboard.com>");
MODULE_DESCRIPTION("Driver for the Vision Components MIPI Module Controller");
MODULE_LICENSE("GPL");
