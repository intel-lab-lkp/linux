// SPDX-License-Identifier: GPL-2.0

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/devfreq.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/pm_opp.h>

struct airoha_devfreq_data {
	struct clk *clk;

	struct devfreq_passive_data gov_data;
};

static int airoha_devfreq_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct airoha_devfreq_data *data = dev_get_drvdata(dev);

	*freq = clk_get_rate(data->clk);

	return 0;
};

static int airoha_devfreq_target(struct device *dev, unsigned long *freq,
				 u32 flags)
{
	struct airoha_devfreq_data *data = dev_get_drvdata(dev);

	return clk_set_rate(data->clk, *freq);
};

static int airoha_devfreq_get_dev_status(struct device *dev,
					 struct devfreq_dev_status *stat)
{
	struct airoha_devfreq_data *data = dev_get_drvdata(dev);

	stat->busy_time = 0;
	stat->total_time = 0;
	stat->current_frequency = clk_get_rate(data->clk);

	return 0;
};

static struct devfreq_dev_profile airoha_devfreq_devfreq_profile = {
	.target = airoha_devfreq_target,
	.get_dev_status = airoha_devfreq_get_dev_status,
	.get_cur_freq = airoha_devfreq_get_cur_freq
};

static int airoha_devfreq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct airoha_devfreq_data *data;
	struct devfreq *devfreq;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(data->clk))
		return dev_err_probe(dev, PTR_ERR(data->clk), "failed to get clk\n");

	ret = devm_pm_opp_of_add_table(dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to parse fab freq thresholds\n");

	dev_set_drvdata(dev, data);

	data->gov_data.parent_type = CPUFREQ_PARENT_DEV;
	devfreq = devm_devfreq_add_device(dev, &airoha_devfreq_devfreq_profile,
					  DEVFREQ_GOV_PASSIVE, &data->gov_data);

	return PTR_ERR_OR_ZERO(devfreq);
};

static const struct of_device_id airoha_devfreq_match_table[] = {
	{ .compatible = "airoha,devfreq" },
	{}
};

static struct platform_driver airoha_devfreq_driver = {
	.probe		= airoha_devfreq_probe,
	.driver		= {
		.name   = "airoha-devfreq",
		.of_match_table = airoha_devfreq_match_table,
	},
};
module_platform_driver(airoha_devfreq_driver);

MODULE_DESCRIPTION("Airoha Devfreq driver");
MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_LICENSE("GPL");
