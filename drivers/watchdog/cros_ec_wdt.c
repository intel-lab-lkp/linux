// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/slab.h>
#include <linux/err.h>
#include <linux/of_device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_data/cros_ec_commands.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>
#include <linux/uaccess.h>

#define CROS_EC_WATCHDOG_DEFAULT_TIME 30 /* seconds */

#define DEV_NAME "cros-ec-wdt-dev"
#define DRV_NAME "cros-ec-wdt-drv"

static int cros_ec_wdt_ping(struct watchdog_device *wdd);
static int cros_ec_wdt_start(struct watchdog_device *wdd);
static int cros_ec_wdt_stop(struct watchdog_device *wdd);
static int cros_ec_wdt_set_timeout(struct watchdog_device *wdd, unsigned int t);

struct cros_ec_wdt_data {
	bool start_on_resume;
	bool keepalive_on;
	struct cros_ec_device *cros_ec;
	struct watchdog_device *wdd;
};
static struct cros_ec_wdt_data wd_data;

static const struct watchdog_info cros_ec_wdt_ident = {
	.options          = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
	.firmware_version = 0,
	.identity         = DRV_NAME,
};

static const struct watchdog_ops cros_ec_wdt_ops = {
	.owner		 = THIS_MODULE,
	.ping		 = cros_ec_wdt_ping,
	.start		 = cros_ec_wdt_start,
	.stop		 = cros_ec_wdt_stop,
	.set_timeout = cros_ec_wdt_set_timeout,
};

static struct watchdog_device cros_ec_wdd = {
	.info = &cros_ec_wdt_ident,
	.ops = &cros_ec_wdt_ops,
	.timeout = CROS_EC_WATCHDOG_DEFAULT_TIME,
	.bootstatus = EC_HANG_DETECT_AP_BOOT_NORMAL
};

static int cros_ec_wdt_send_hang_detect(struct cros_ec_wdt_data *wd_data,
					uint16_t command,
					uint16_t reboot_timeout_sec,
					uint32_t *status)
{
	int ret;

	struct {
		struct cros_ec_command msg;
		union {
			struct ec_params_hang_detect req;
			struct ec_response_hang_detect resp;
		};
	} __packed buf = {
		.msg = {
			.version = 0,
			.command = EC_CMD_HANG_DETECT,
			.insize  = (command == EC_HANG_DETECT_CMD_GET_STATUS) ?
				   sizeof(struct ec_response_hang_detect) :
				   0,
			.outsize = sizeof(struct ec_params_hang_detect),
		},
		.req = {
			.command = command,
			.reboot_timeout_sec = reboot_timeout_sec,
		}
	};

	ret = cros_ec_cmd_xfer_status(wd_data->cros_ec, &buf.msg);
	if (ret < 0) {
		dev_warn(wd_data->wdd->parent,
				 "cros_ec_cmd_xfer_status failed(%d) command (%04x) reboot_timeout_sec(%ds)",
				 ret, command, reboot_timeout_sec);
		return ret;
	}

	if (status && (command == EC_HANG_DETECT_CMD_GET_STATUS)) {
		*status = buf.resp.status;
		dev_info(wd_data->wdd->parent, "EC Watchdog boot status (%d)",
				 buf.resp.status);
	}

	return 0;
}

static int cros_ec_wdt_ping(struct watchdog_device *wdd)
{
	struct cros_ec_wdt_data *wd_data = watchdog_get_drvdata(wdd);
	int ret;

	ret = cros_ec_wdt_send_hang_detect(wd_data, EC_HANG_DETECT_CMD_RELOAD,
					   0, NULL);
	if (ret < 0)
		dev_err(wdd->parent, "%s failed (%d)", __func__, ret);
	else
		wd_data->keepalive_on = true;

	return ret;
}

static int cros_ec_wdt_start(struct watchdog_device *wdd)
{
	struct cros_ec_wdt_data *wd_data = watchdog_get_drvdata(wdd);
	int ret = 0;

	/* Prepare watchdog on EC side */
	ret = cros_ec_wdt_send_hang_detect(wd_data,
					EC_HANG_DETECT_CMD_SET_TIMEOUT,
					wdd->timeout,
					NULL);
	if (ret < 0)
		dev_err(wdd->parent, "%s failed (%d)", __func__, ret);

	return ret;
}

static int cros_ec_wdt_stop(struct watchdog_device *wdd)
{
	struct cros_ec_wdt_data *wd_data = watchdog_get_drvdata(wdd);
	int ret = 0;

	if (wd_data->keepalive_on) {
		wd_data->keepalive_on = false;
		ret = cros_ec_wdt_send_hang_detect(wd_data, EC_HANG_DETECT_CMD_CANCEL,
						0, NULL);
		if (ret < 0)
			dev_err(wdd->parent, "%s failed (%d)", __func__, ret);
	}

	return ret;
}

static int cros_ec_wdt_set_timeout(struct watchdog_device *wdd, unsigned int t)
{
	struct cros_ec_wdt_data *wd_data = watchdog_get_drvdata(wdd);
	int ret;

	if (t < EC_HANG_DETECT_MIN_TIMEOUT) {
		dev_err(wdd->parent,
				"%s failed, requested timeout is lower than min(%d < %d)",
				__func__, t, EC_HANG_DETECT_MIN_TIMEOUT);
		return -EINVAL;
	}

	ret = cros_ec_wdt_send_hang_detect(wd_data,
					   EC_HANG_DETECT_CMD_SET_TIMEOUT,
					   t, NULL);
	if (ret < 0)
		dev_err(wdd->parent, "%s failed (%d)", __func__, ret);
	else
		wdd->timeout = t;

	return ret;
}

static int cros_ec_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	/* We need to get a reference to cros_ec_devices
	 * (provides communication layer) which is a parent of
	 * the cros-ec-dev (our parent)
	 */
	struct cros_ec_device *cros_ec = dev_get_drvdata(dev->parent->parent);
	int ret = 0;
	uint32_t bootstatus;

	if (!cros_ec) {
		ret = -ENODEV;
		dev_err_probe(dev, ret, "There is no coresponding EC device!");
		return ret;
	}

	cros_ec_wdd.parent = &pdev->dev;
	wd_data.cros_ec = cros_ec;
	wd_data.wdd = &cros_ec_wdd;
	wd_data.start_on_resume = false;
	wd_data.keepalive_on = false;
	wd_data.wdd->timeout = CROS_EC_WATCHDOG_DEFAULT_TIME;

	watchdog_stop_on_reboot(&cros_ec_wdd);
	watchdog_stop_on_unregister(&cros_ec_wdd);
	watchdog_set_drvdata(&cros_ec_wdd, &wd_data);
	platform_set_drvdata(pdev, &wd_data);

	/* Get current AP boot status */
	ret = cros_ec_wdt_send_hang_detect(&wd_data, EC_HANG_DETECT_CMD_GET_STATUS, 0,
					   &bootstatus);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Couldn't get AP boot status from EC");
		return ret;
	}

	/*
	 * If bootstatus is not EC_HANG_DETECT_AP_BOOT_NORMAL
	 * it mens EC has rebooted the AP due to watchdog timeout.
	 * Lets translate it to watchdog core status code.
	 */
	if (bootstatus != EC_HANG_DETECT_AP_BOOT_NORMAL)
		wd_data.wdd->bootstatus = WDIOF_CARDRESET;

	ret = watchdog_register_device(&cros_ec_wdd);
	if (ret < 0)
		dev_err_probe(dev, ret, "Couldn't get AP boot status from EC");

	return ret;
}

static int cros_ec_wdt_remove(struct platform_device *pdev)
{
	struct cros_ec_wdt_data *wd_data = platform_get_drvdata(pdev);

	watchdog_unregister_device(wd_data->wdd);

	return 0;
}

static void cros_ec_wdt_shutdown(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cros_ec_wdt_data *wd_data = platform_get_drvdata(pdev);
	int ret;

	/*
	 * Clean only bootstatus flag.
	 * EC wdt is are already stopped by watchdog framework.
	 */
	ret = cros_ec_wdt_send_hang_detect(wd_data,
					   EC_HANG_DETECT_CMD_CLEAR_STATUS, 0, NULL);
	if (ret < 0)
		dev_err(dev, "%s failed (%d)", __func__, ret);

	watchdog_unregister_device(wd_data->wdd);
}

static int __maybe_unused cros_ec_wdt_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct device *dev = &pdev->dev;
	struct cros_ec_wdt_data *wd_data = platform_get_drvdata(pdev);
	int ret;

	if (watchdog_active(wd_data->wdd)) {
		ret = cros_ec_wdt_send_hang_detect(wd_data,
						   EC_HANG_DETECT_CMD_CANCEL, 0, NULL);
		if (ret < 0)
			dev_err(dev, "%s failed (%d)", __func__, ret);
		wd_data->start_on_resume = true;
	}

	return 0;
}

static int __maybe_unused cros_ec_wdt_resume(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cros_ec_wdt_data *wd_data = platform_get_drvdata(pdev);
	int ret;

	/* start_on_resume is only set if watchdog was active
	 * when device was going to sleep
	 */
	if (wd_data->start_on_resume) {
		/* On resume we just need to setup a EC watchdog the same way as
		 * in cros_ec_wdt_start(). When userspace resumes from suspend
		 * the watchdog app should just start petting the watchdog again.
		 */
		ret = cros_ec_wdt_start(wd_data->wdd);
		if (ret < 0)
			dev_err(dev, "%s failed (%d)", __func__, ret);

		wd_data->start_on_resume = false;
	}

	return 0;
}

static struct platform_driver cros_ec_wdt_driver = {
	.probe		= cros_ec_wdt_probe,
	.remove		= cros_ec_wdt_remove,
	.shutdown	= cros_ec_wdt_shutdown,
	.suspend	= pm_ptr(cros_ec_wdt_suspend),
	.resume		= pm_ptr(cros_ec_wdt_resume),
	.driver		= {
		.name	= DRV_NAME,
	},
};

module_platform_driver(cros_ec_wdt_driver);

MODULE_ALIAS("platform:" DRV_NAME);
MODULE_DESCRIPTION("Cros EC Watchdog Device Driver");
MODULE_LICENSE("GPL");
