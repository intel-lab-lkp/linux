// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019, 2024 NXP
 *
 */

/*
 * The i.MX8QXP SoC contains the Secure Non-Volatile Storage (SNVS) block. This
 * block can detect specific hardware attacks.This block can only be accessible
 * using the SCFW API.
 *
 * This module interact with the SCU which relay request to/from the SNVS block
 * to detect if security violation occurred.
 *
 * The module exports an API to add processing when a SV is detected:
 *  - register_imx_secvio_sc_notifier
 *  - unregister_imx_secvio_sc_notifier
 *  - imx_secvio_sc_check_state
 *  - imx_secvio_sc_clear_state
 *  - imx_secvio_sc_enable_irq
 *  - imx_secvio_sc_disable_irq
 */

#include <dt-bindings/firmware/imx/rsrc.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/nvmem-consumer.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#include <linux/firmware/imx/ipc.h>
#include <linux/firmware/imx/sci.h>
#include <linux/firmware/imx/svc/seco.h>
#include <linux/firmware/imx/svc/rm.h>
#include <soc/imx/imx-secvio-sc.h>

/* Reference on the driver_device */
static struct device *imx_secvio_sc_dev;

/* Register IDs for sc_seco_secvio_config API */
#define HPSVS_ID 0x18
#define LPS_ID 0x4c
#define LPTDS_ID 0xa4
#define HPVIDR_ID 0xf8

#define SECO_MINOR_VERSION_SUPPORT_SECVIO_TAMPER 0x53
#define SECO_VERSION_MINOR_MASK GENMASK(15, 0)

/* Notifier list for new CB */
static BLOCKING_NOTIFIER_HEAD(imx_secvio_sc_notifier_chain);

int register_imx_secvio_sc_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&imx_secvio_sc_notifier_chain,
						nb);
}
EXPORT_SYMBOL(register_imx_secvio_sc_notifier);

int unregister_imx_secvio_sc_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&imx_secvio_sc_notifier_chain,
						  nb);
}
EXPORT_SYMBOL(unregister_imx_secvio_sc_notifier);

static void if_imx_scu_irq_register_notifier(void *nb)
{
	imx_scu_irq_register_notifier(nb);
}

static void if_unregister_imx_secvio_sc_notifier(void *nb)
{
	unregister_imx_secvio_sc_notifier(nb);
}

static
int imx_secvio_sc_notifier_call_chain(struct secvio_sc_notifier_info *info)
{
	return blocking_notifier_call_chain(&imx_secvio_sc_notifier_chain, 0,
					    (void *)info);
}

int imx_secvio_sc_get_state(struct device *dev,
			    struct secvio_sc_notifier_info *info)
{
	int ret, err = 0;
	struct imx_secvio_sc_data *data;

	dev = imx_secvio_sc_dev;
	if (!dev)
		return -EINVAL;

	data = dev_get_drvdata(dev);

	/* Read secvio status */
	ret = imx_sc_seco_secvio_config(data->ipc_handle, HPSVS_ID, SECVIO_CONFIG_READ,
					&info->hpsvs, NULL, NULL, NULL, NULL, 1);
	if (ret) {
		err = ret;
		dev_err(dev, "Fail read secvio config status %d\n", ret);
	}
	info->hpsvs &= HPSVS_ALL_SV_MASK;

	/* Read tampers status */
	ret = imx_sc_seco_secvio_config(data->ipc_handle, LPS_ID, SECVIO_CONFIG_READ,
					&info->lps, NULL, NULL, NULL, NULL, 1);
	if (ret) {
		err = ret;
		dev_err(dev, "Fail read tamper 1 status: %d\n", ret);
	}
	info->lps &= LPS_ALL_TP_MASK;

	ret = imx_sc_seco_secvio_config(data->ipc_handle, LPTDS_ID, SECVIO_CONFIG_READ,
					&info->lptds, NULL, NULL, NULL, NULL, 1);
	if (ret) {
		err = ret;
		dev_err(dev, "Fail read  tamper 2 status: %d\n", ret);
	}
	info->lptds &= LPTDS_ALL_TP_MASK;

	dev_dbg(dev, "Status: %.8x, %.8x, %.8x\n", info->hpsvs,
		info->lps, info->lptds);

	return err;
}
EXPORT_SYMBOL(imx_secvio_sc_get_state);

int imx_secvio_sc_check_state(struct device *dev)
{
	struct secvio_sc_notifier_info info;
	int ret;

	dev = imx_secvio_sc_dev;

	ret = imx_secvio_sc_get_state(dev, &info);
	if (ret) {
		dev_err(dev, "Failed to get secvio state\n");
		return ret;
	}

	/* Call chain of CB registered to this module if status detected */
	if (info.hpsvs || info.lps || info.lptds)
		if (imx_secvio_sc_notifier_call_chain(&info))
			dev_warn(dev,
				 "Issues when calling the notifier chain\n");

	return ret;
}
EXPORT_SYMBOL(imx_secvio_sc_check_state);

static int imx_secvio_sc_disable_irq(struct device *dev)
{
	int ret;

	if (!dev)
		return -EINVAL;

	/* Disable the IRQ */
	ret = imx_scu_irq_group_enable(IMX_SC_IRQ_GROUP_WAKE, IMX_SC_IRQ_SECVIO,
				       false);
	if (ret) {
		dev_err(dev, "Cannot disable SCU IRQ: %d\n", ret);
		return ret;
	}

	return ret;
}

static int imx_secvio_sc_enable_irq(struct device *dev)
{
	int ret = 0, err;
	u32 irq_status;
	struct imx_secvio_sc_data *data;

	if (!dev)
		return -EINVAL;

	data = dev_get_drvdata(dev);

	/* Enable the IRQ */
	ret = imx_scu_irq_group_enable(IMX_SC_IRQ_GROUP_WAKE, IMX_SC_IRQ_SECVIO,
				       true);
	if (ret) {
		dev_err(dev, "Cannot enable SCU IRQ: %d\n", ret);
		goto exit;
	}

	/* Enable interrupt */
	ret = imx_sc_seco_secvio_enable(data->ipc_handle);
	if (ret) {
		dev_err(dev, "Cannot enable SNVS irq: %d\n", ret);
		goto exit;
	}

	/* Unmask interrupt */
	ret = imx_scu_irq_get_status(IMX_SC_IRQ_GROUP_WAKE, &irq_status);
	if (ret) {
		dev_err(dev, "Cannot unmask irq: %d\n", ret);
		goto exit;
	}

exit:
	if (ret) {
		err = imx_secvio_sc_disable_irq(dev);
		if (err)
			dev_warn(dev, "Failed to disable the IRQ\n");
	}

	return ret;
}

static int imx_secvio_sc_notify(struct notifier_block *nb,
				unsigned long event, void *group)
{
	struct imx_secvio_sc_data *data =
				container_of(nb, struct imx_secvio_sc_data,
					     irq_nb);
	struct device *dev = data->dev;
	int ret;

	/* Filter event for us */
	if (!((event & IMX_SC_IRQ_SECVIO) &&
	      (*(u8 *)group == IMX_SC_IRQ_GROUP_WAKE)))
		return 0;

	dev_warn(dev, "secvio security violation detected\n");

	ret = imx_secvio_sc_check_state(dev);

	/* Re-enable interrupt */
	ret = imx_secvio_sc_enable_irq(dev);
	if (ret)
		dev_err(dev, "Failed to enable IRQ\n");

	return ret;
}

int imx_secvio_sc_clear_state(struct device *dev, u32 hpsvs, u32 lps, u32 lptds)
{
	int ret;
	struct imx_secvio_sc_data *data;

	dev = imx_secvio_sc_dev;
	if (!dev)
		return -EINVAL;

	data = dev_get_drvdata(dev);

	ret = imx_sc_seco_secvio_config(data->ipc_handle, HPSVS_ID, SECVIO_CONFIG_WRITE,
					&hpsvs, NULL, NULL, NULL, NULL, 1);
	if (ret) {
		dev_err(dev, "Fail to clear secvio status: %d\n", ret);
		return ret;
	}

	ret = imx_sc_seco_secvio_config(data->ipc_handle, LPS_ID, SECVIO_CONFIG_WRITE,
					&lps, NULL, NULL, NULL, NULL, 1);
	if (ret) {
		dev_err(dev, "Fail to clear tamper 1 status: %d\n", ret);
		return ret;
	}

	ret = imx_sc_seco_secvio_config(data->ipc_handle, LPTDS_ID, SECVIO_CONFIG_WRITE,
					&lptds, NULL, NULL, NULL, NULL, 1);
	if (ret) {
		dev_err(dev, "Fail to clear tamper 2 status: %d\n", ret);
		return ret;
	}

	return ret;
}
EXPORT_SYMBOL(imx_secvio_sc_clear_state);

static int report_to_user_notify(struct notifier_block *nb,
				 unsigned long status, void *notif_info)
{
	struct secvio_sc_notifier_info *info = notif_info;
	struct imx_secvio_sc_data *data =
				container_of(nb, struct imx_secvio_sc_data,
					     report_nb);
	struct device *dev = data->dev;

	/* Information about the security violation */
	if (info->hpsvs & HPSVS_LP_SEC_VIO_MASK)
		dev_info(dev, "SNVS secvio: LPSV\n");
	if (info->hpsvs & HPSVS_SW_LPSV_MASK)
		dev_info(dev, "SNVS secvio: SW LPSV\n");
	if (info->hpsvs & HPSVS_SW_FSV_MASK)
		dev_info(dev, "SNVS secvio: SW FSV\n");
	if (info->hpsvs & HPSVS_SW_SV_MASK)
		dev_info(dev, "SNVS secvio: SW SV\n");
	if (info->hpsvs & HPSVS_SV5_MASK)
		dev_info(dev, "SNVS secvio: SV 5\n");
	if (info->hpsvs & HPSVS_SV4_MASK)
		dev_info(dev, "SNVS secvio: SV 4\n");
	if (info->hpsvs & HPSVS_SV3_MASK)
		dev_info(dev, "SNVS secvio: SV 3\n");
	if (info->hpsvs & HPSVS_SV2_MASK)
		dev_info(dev, "SNVS secvio: SV 2\n");
	if (info->hpsvs & HPSVS_SV1_MASK)
		dev_info(dev, "SNVS secvio: SV 1\n");
	if (info->hpsvs & HPSVS_SV0_MASK)
		dev_info(dev, "SNVS secvio: SV 0\n");

	/* Information about the tampers */
	if (info->lps & LPS_ESVD_MASK)
		dev_info(dev, "SNVS tamper: External SV\n");
	if (info->lps & LPS_ET2D_MASK)
		dev_info(dev, "SNVS tamper: Tamper 2\n");
	if (info->lps & LPS_ET1D_MASK)
		dev_info(dev, "SNVS tamper: Tamper 1\n");
	if (info->lps & LPS_WMT2D_MASK)
		dev_info(dev, "SNVS tamper: Wire Mesh 2\n");
	if (info->lps & LPS_WMT1D_MASK)
		dev_info(dev, "SNVS tamper: Wire Mesh 1\n");
	if (info->lps & LPS_VTD_MASK)
		dev_info(dev, "SNVS tamper: Voltage\n");
	if (info->lps & LPS_TTD_MASK)
		dev_info(dev, "SNVS tamper: Temperature\n");
	if (info->lps & LPS_CTD_MASK)
		dev_info(dev, "SNVS tamper: Clock\n");
	if (info->lps & LPS_PGD_MASK)
		dev_info(dev, "SNVS tamper: Power Glitch\n");
	if (info->lps & LPS_MCR_MASK)
		dev_info(dev, "SNVS tamper: Monotonic Counter rollover\n");
	if (info->lps & LPS_SRTCR_MASK)
		dev_info(dev, "SNVS tamper: Secure RTC rollover\n");
	if (info->lps & LPS_LPTA_MASK)
		dev_info(dev, "SNVS tamper: Time alarm\n");

	if (info->lptds & LPTDS_ET10D_MASK)
		dev_info(dev, "SNVS tamper: Tamper 10\n");
	if (info->lptds & LPTDS_ET9D_MASK)
		dev_info(dev, "SNVS tamper: Tamper 9\n");
	if (info->lptds & LPTDS_ET8D_MASK)
		dev_info(dev, "SNVS tamper: Tamper 8\n");
	if (info->lptds & LPTDS_ET7D_MASK)
		dev_info(dev, "SNVS tamper: Tamper 7\n");
	if (info->lptds & LPTDS_ET6D_MASK)
		dev_info(dev, "SNVS tamper: Tamper 6\n");
	if (info->lptds & LPTDS_ET5D_MASK)
		dev_info(dev, "SNVS tamper: Tamper 5\n");
	if (info->lptds & LPTDS_ET4D_MASK)
		dev_info(dev, "SNVS tamper: Tamper 4\n");
	if (info->lptds & LPTDS_ET3D_MASK)
		dev_info(dev, "SNVS tamper: Tamper 3\n");

	return 0;
}

static void if_imx_secvio_sc_disable_irq(void *dev)
{
	imx_secvio_sc_disable_irq(dev);
}

static int imx_secvio_sc_open(struct inode *node, struct file *filp)
{
	filp->private_data = node->i_private;

	return 0;
}

static long imx_secvio_sc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct device *dev = file->private_data;
	struct secvio_sc_notifier_info info;
	int ret;

	switch (cmd) {
	case IMX_SECVIO_SC_GET_STATE:
		ret = imx_secvio_sc_get_state(dev, &info);
		if (ret)
			return ret;

		ret = copy_to_user((void *)arg, &info, sizeof(info));
		if (ret) {
			dev_err(dev, "Fail to copy info to user\n");
			return -EFAULT;
		}
		break;
	case IMX_SECVIO_SC_CHECK_STATE:
		ret = imx_secvio_sc_check_state(dev);
		if (ret)
			return ret;
		break;
	case IMX_SECVIO_SC_CLEAR_STATE:
		ret = copy_from_user(&info, (void *)arg, sizeof(info));
		if (ret) {
			dev_err(dev, "Fail to copy info from user\n");
			return -EFAULT;
		}

		ret = imx_secvio_sc_clear_state(dev, info.hpsvs, info.lps,
						    info.lptds);
		if (ret)
			return ret;
		break;
	default:
		ret = -ENOIOCTLCMD;
	}

	return ret;
}

static const struct file_operations imx_secvio_sc_fops = {
	.owner = THIS_MODULE,
	.open = imx_secvio_sc_open,
	.unlocked_ioctl = imx_secvio_sc_ioctl,
};

static void if_misc_deregister(void *miscdevice)
{
	misc_deregister(miscdevice);
}

static int imx_secvio_sc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct imx_secvio_sc_data *data;
	u32 seco_version = 0;
	bool own_secvio;
	u32 irq_status;
	int ret;

	/* Allocate private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if (!devres_open_group(dev, NULL, GFP_KERNEL))
		return -ENOMEM;

	data->dev = dev;

	dev_set_drvdata(dev, data);

	data->nvmem = devm_nvmem_device_get(dev, NULL);
	if (IS_ERR(data->nvmem)) {
		ret = PTR_ERR(data->nvmem);

		if (ret != -EPROBE_DEFER)
			dev_err_probe(dev, ret, "Failed to retrieve nvmem\n");

		goto clean;
	}

	/* Get a handle */
	ret = imx_scu_get_handle(&data->ipc_handle);
	if (ret) {
		dev_err(dev, "cannot get handle to scu: %d\n", ret);
		goto clean;
	}

	/* Check the version of the SECO */
	ret = imx_sc_seco_build_info(data->ipc_handle, &seco_version, NULL);
	if (ret) {
		dev_err(dev, "Failed to get seco version\n");
		goto clean;
	}

	if ((seco_version & SECO_VERSION_MINOR_MASK) <
	     SECO_MINOR_VERSION_SUPPORT_SECVIO_TAMPER) {
		dev_err(dev, "SECO version %.8x doesn't support all secvio\n",
			seco_version);
		ret = -EOPNOTSUPP;
		goto clean;
	}

	/* Init debug FS */
	ret = imx_secvio_sc_debugfs(dev);
	if (ret) {
		dev_err(dev, "Failed to set debugfs\n");
		goto clean;
	}

	/* Check we own the SECVIO */
	ret = imx_sc_rm_is_resource_owned(data->ipc_handle, IMX_SC_R_SECVIO);
	if (ret < 0) {
		dev_err(dev, "Failed to retrieve secvio ownership\n");
		goto clean;
	}

	own_secvio = ret > 0;
	if (!own_secvio) {
		dev_err(dev, "Secvio resource is not owned\n");
		ret = -EPERM;
		goto clean;
	}

	/* Check IRQ exists and enable it */
	ret = imx_scu_irq_get_status(IMX_SC_IRQ_GROUP_WAKE, &irq_status);
	if (ret) {
		dev_err(dev, "Cannot get IRQ state: %d\n", ret);
		goto clean;
	}

	ret = imx_secvio_sc_enable_irq(dev);
	if (ret) {
		dev_err(dev, "Failed to enable IRQ\n");
		goto clean;
	}

	ret = devm_add_action_or_reset(dev, if_imx_secvio_sc_disable_irq, dev);
	if (ret) {
		dev_err(dev, "Failed to add managed action to disable IRQ\n");
		goto clean;
	}

	/* Register the notifier for IRQ from SNVS */
	data->irq_nb.notifier_call = imx_secvio_sc_notify;
	ret = imx_scu_irq_register_notifier(&data->irq_nb);
	if (ret) {
		dev_err(dev, "Failed to register IRQ notification handler\n");
		goto clean;
	}

	ret = devm_add_action_or_reset(dev, if_imx_scu_irq_register_notifier,
				       &data->irq_nb);
	if (ret) {
		dev_err(dev, "Failed to add action to remove irq notif\n");
		goto clean;
	}

	/* Register the notification for reporting to user */
	data->report_nb.notifier_call = report_to_user_notify;
	ret = register_imx_secvio_sc_notifier(&data->report_nb);
	if (ret) {
		dev_err(dev, "Failed to register report notif handler\n");
		goto clean;
	}

	ret = devm_add_action_or_reset(dev, if_unregister_imx_secvio_sc_notifier,
				       &data->report_nb);
	if (ret) {
		dev_err(dev, "Failed to add action to remove report notif\n");
		goto clean;
	}

	/* Register misc device for IOCTL */
	data->miscdev.name = devm_kstrdup(dev, "secvio-sc", GFP_KERNEL);
	data->miscdev.minor = MISC_DYNAMIC_MINOR;
	data->miscdev.fops = &imx_secvio_sc_fops;
	data->miscdev.parent = dev;
	ret = misc_register(&data->miscdev);
	if (ret) {
		dev_err(dev, "failed to register misc device\n");
		return ret;
	}

	ret = devm_add_action_or_reset(dev, if_misc_deregister, &data->miscdev);
	if (ret) {
		dev_err(dev, "Failed to add action to unregister miscdev\n");
		goto clean;
	}

	imx_secvio_sc_dev = dev;

	/* Process current state of the secvio and tampers */
	imx_secvio_sc_check_state(dev);

	devres_remove_group(dev, NULL);

	return ret;

clean:
	devres_release_group(dev, NULL);

	return ret;
}

static const struct of_device_id imx_secvio_sc_dt_ids[] = {
	{ .compatible = "fsl,imx-sc-secvio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx_secvio_sc_dt_ids);

static struct platform_driver imx_secvio_sc_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name	= "imx-secvio-sc",
		.of_match_table = imx_secvio_sc_dt_ids,
	},
	.probe		= imx_secvio_sc_probe,
};
module_platform_driver(imx_secvio_sc_driver);

MODULE_AUTHOR("Franck LENORMAND <franck.lenormand@nxp.com>");
MODULE_DESCRIPTION("NXP i.MX driver to handle SNVS secvio irq sent by SCFW");
MODULE_LICENSE("GPL");
