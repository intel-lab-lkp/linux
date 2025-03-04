// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2025 Vasiliy Doylov <nekocwd@mainlining.org>

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-event.h>

#define LC898217XC_NAME "lc898217xc"
/* Actuator has 11 bit resolution */
#define LC898217XC_MAX_FOCUS_POS (2048 - 1)
#define LC898217XC_MIN_FOCUS_POS 0
#define LC898217XC_FOCUS_STEPS 1

#define LC898217XC_MSB_ADDR 132

static const char *const lc898217xc_supply_names[] = {
	"vcc",
};

struct lc898217xc {
	struct regulator_bulk_data supplies[ARRAY_SIZE(lc898217xc_supply_names)];
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *focus;
	struct v4l2_subdev sd;
};

static inline struct lc898217xc *sd_to_lc898217xc(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct lc898217xc, sd);
}

static int lc898217xc_set_dac(struct lc898217xc *lc898217xc, u16 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&lc898217xc->sd);

	return i2c_smbus_write_word_swapped(client, LC898217XC_MSB_ADDR, val);
}

static int __maybe_unused lc898217xc_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct lc898217xc *lc898217xc = sd_to_lc898217xc(sd);

	regulator_bulk_disable(ARRAY_SIZE(lc898217xc_supply_names),
			       lc898217xc->supplies);

	return 0;
}

static int __maybe_unused lc898217xc_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct lc898217xc *lc898217xc = sd_to_lc898217xc(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(lc898217xc_supply_names),
				    lc898217xc->supplies);

	if (ret < 0) {
		dev_err(dev, "failed to enable regulators\n");
		return ret;
	}

	usleep_range(8000, 10000);

	return ret;
}

static int lc898217xc_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct lc898217xc *lc898217xc =
		container_of(ctrl->handler, struct lc898217xc, ctrls);

	if (ctrl->id == V4L2_CID_FOCUS_ABSOLUTE)
		return lc898217xc_set_dac(lc898217xc, ctrl->val);

	return 0;
}

static const struct v4l2_ctrl_ops lc898217xc_ctrl_ops = {
	.s_ctrl = lc898217xc_set_ctrl,
};

static int lc898217xc_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int lc898217xc_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pm_runtime_mark_last_busy(sd->dev);
	pm_runtime_put_autosuspend(sd->dev);

	return 0;
}

static const struct v4l2_subdev_internal_ops lc898217xc_int_ops = {
	.open = lc898217xc_open,
	.close = lc898217xc_close,
};

static const struct v4l2_subdev_core_ops lc898217xc_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops lc898217xc_ops = {
	.core = &lc898217xc_core_ops,
};

static int lc898217xc_init_controls(struct lc898217xc *lc898217xc)
{
	struct v4l2_ctrl_handler *hdl = &lc898217xc->ctrls;
	const struct v4l2_ctrl_ops *ops = &lc898217xc_ctrl_ops;

	v4l2_ctrl_handler_init(hdl, 1);

	lc898217xc->focus = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FOCUS_ABSOLUTE,
					      LC898217XC_MIN_FOCUS_POS,
					      LC898217XC_MAX_FOCUS_POS,
					      LC898217XC_FOCUS_STEPS, 0);

	if (hdl->error)
		return hdl->error;

	lc898217xc->sd.ctrl_handler = hdl;

	return 0;
}

static int lc898217xc_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct lc898217xc *lc898217xc;
	unsigned int i;
	int ret;

	lc898217xc = devm_kzalloc(dev, sizeof(*lc898217xc), GFP_KERNEL);
	if (!lc898217xc)
		return -ENOMEM;

	/* Initialize subdev */
	v4l2_i2c_subdev_init(&lc898217xc->sd, client, &lc898217xc_ops);

	for (i = 0; i < ARRAY_SIZE(lc898217xc_supply_names); i++)
		lc898217xc->supplies[i].supply = lc898217xc_supply_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(lc898217xc_supply_names),
				      lc898217xc->supplies);

	if (ret) {
		dev_err(dev, "failed to get regulators\n");
		return ret;
	}

	/* Initialize controls */
	ret = lc898217xc_init_controls(lc898217xc);
	if (ret)
		goto err_free_handler;

	/* Initialize subdev */
	lc898217xc->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
				V4L2_SUBDEV_FL_HAS_EVENTS;
	lc898217xc->sd.internal_ops = &lc898217xc_int_ops;

	ret = media_entity_pads_init(&lc898217xc->sd.entity, 0, NULL);
	if (ret < 0)
		goto err_free_handler;

	lc898217xc->sd.entity.function = MEDIA_ENT_F_LENS;

	pm_runtime_enable(dev);
	ret = v4l2_async_register_subdev(&lc898217xc->sd);

	if (ret < 0) {
		dev_err(dev, "failed to register V4L2 subdev: %d", ret);
		goto err_power_off;
	}

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_idle(dev);

	return 0;

err_power_off:
	pm_runtime_disable(dev);
	media_entity_cleanup(&lc898217xc->sd.entity);
err_free_handler:
	v4l2_ctrl_handler_free(&lc898217xc->ctrls);

	return ret;
}

static void lc898217xc_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct lc898217xc *lc898217xc = sd_to_lc898217xc(sd);
	struct device *dev = &client->dev;

	v4l2_async_unregister_subdev(&lc898217xc->sd);
	v4l2_ctrl_handler_free(&lc898217xc->ctrls);
	media_entity_cleanup(&lc898217xc->sd.entity);
	pm_runtime_disable(dev);
}

static const struct of_device_id lc898217xc_of_table[] = {
	{ .compatible = "onnn,lc898217xc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, lc898217xc_of_table);

static const struct dev_pm_ops lc898217xc_pm_ops = {
	SET_RUNTIME_PM_OPS(lc898217xc_runtime_suspend,
			   lc898217xc_runtime_resume, NULL)
};

static struct i2c_driver lc898217xc_i2c_driver = {
	.driver = {
		.name = LC898217XC_NAME,
		.pm = &lc898217xc_pm_ops,
		.of_match_table = lc898217xc_of_table,
	},
	.probe = lc898217xc_probe,
	.remove = lc898217xc_remove,
};
module_i2c_driver(lc898217xc_i2c_driver);

MODULE_AUTHOR("Vasiliy Doylov <nekocwd@mainlining.org>");
MODULE_DESCRIPTION("Onsemi LC898217XC VCM driver");
MODULE_LICENSE("GPL");
