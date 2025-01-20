// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2024 NXP
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/firmware.h>
#include <linux/firmware/imx/se_api.h>
#include <linux/genalloc.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sys_soc.h>

#include "ele_base_msg.h"
#include "ele_common.h"
#include "se_ctrl.h"

#define MAX_SOC_INFO_DATA_SZ		256
#define MBOX_TX_NAME			"tx"
#define MBOX_RX_NAME			"rx"
#define SE_TYPE_STR_HSM			"hsm"
#define SE_TYPE_ID_HSM			0x2

struct se_fw_img_name {
	const u8 *prim_fw_nm_in_rfs;
	const u8 *seco_fw_nm_in_rfs;
};

struct se_fw_load_info {
	const struct se_fw_img_name *se_fw_img_nm;
	bool is_fw_loaded;
	bool imem_mgmt;
	struct se_imem_buf imem;
};

struct se_if_node_info {
	u8 se_if_id;
	u8 se_if_did;
	struct se_if_defines if_defs;
	u8 *pool_name;
	bool reserved_dma_ranges;
};

/* contains fixed information */
struct se_if_node_info_list {
	const u8 num_mu;
	const u16 soc_id;
	bool soc_register;
	int (*se_fetch_soc_info)(struct se_if_priv *priv, void *data);
	const struct se_fw_img_name se_fw_img_nm;
	const struct se_if_node_info info[];
};

struct se_var_info {
	u16 soc_rev;
	struct se_fw_load_info load_fw;
};

static struct se_var_info var_se_info = {
	.soc_rev = 0,
	.load_fw = {
		.is_fw_loaded = true,
		.imem_mgmt = false,
	},
};

static struct se_if_node_info_list imx8ulp_info = {
	.num_mu = 1,
	.soc_id = SOC_ID_OF_IMX8ULP,
	.soc_register = true,
	.se_fetch_soc_info = ele_fetch_soc_info,
	.se_fw_img_nm = {
		.prim_fw_nm_in_rfs = IMX_ELE_FW_DIR
			"mx8ulpa2-ahab-container.img",
		.seco_fw_nm_in_rfs = IMX_ELE_FW_DIR
			"mx8ulpa2ext-ahab-container.img",
	},
	.info = {
			{
			.se_if_id = 0,
			.se_if_did = 7,
			.if_defs = {
				.se_if_type = SE_TYPE_ID_HSM,
				.se_instance_id = 0,
				.cmd_tag = 0x17,
				.rsp_tag = 0xe1,
				.success_tag = ELE_SUCCESS_IND,
				.base_api_ver = MESSAGING_VERSION_6,
				.fw_api_ver = MESSAGING_VERSION_7,
			},
			.pool_name = "sram",
			.reserved_dma_ranges = true,
			},
	},
};

static struct se_if_node_info_list imx93_info = {
	.num_mu = 1,
	.soc_id = SOC_ID_OF_IMX93,
	.soc_register = false,
	.se_fetch_soc_info = ele_fetch_soc_info,
	.se_fw_img_nm = {
		.prim_fw_nm_in_rfs = NULL,
		.seco_fw_nm_in_rfs = NULL,
	},
	.info = {
			{
			.se_if_id = 2,
			.se_if_did = 3,
			.if_defs = {
				.se_if_type = SE_TYPE_ID_HSM,
				.se_instance_id = 0,
				.cmd_tag = 0x17,
				.rsp_tag = 0xe1,
				.success_tag = ELE_SUCCESS_IND,
				.base_api_ver = MESSAGING_VERSION_6,
				.fw_api_ver = MESSAGING_VERSION_7,
			},
			.reserved_dma_ranges = true,
			},
	},
};

static const struct of_device_id se_match[] = {
	{ .compatible = "fsl,imx8ulp-se", .data = (void *)&imx8ulp_info},
	{ .compatible = "fsl,imx93-se", .data = (void *)&imx93_info},
	{},
};

static struct se_fw_load_info *get_load_fw_instance(struct se_if_priv *priv)
{
	return &var_se_info.load_fw;
}

static int se_soc_info(struct se_if_priv *priv)
{
	const struct se_if_node_info_list *info_list = device_get_match_data(priv->dev);
	struct se_fw_load_info *load_fw = get_load_fw_instance(priv);
	struct soc_device_attribute *attr;
	struct ele_dev_info *s_info;
	struct soc_device *sdev;
	u8 data[MAX_SOC_INFO_DATA_SZ];
	int err = 0;

	/* This function should be called once.
	 * Check if the se_soc_rev is zero to continue.
	 */
	if (var_se_info.soc_rev)
		return err;

	if (info_list->se_fetch_soc_info) {
		err = info_list->se_fetch_soc_info(priv, &data);
		if (err < 0) {
			dev_err(priv->dev, "Failed to fetch SoC Info.");
			return err;
		}
		s_info = (void *)data;
		var_se_info.soc_rev = s_info->d_info.soc_rev;
		load_fw->imem.state = s_info->d_addn_info.imem_state;
	} else {
		dev_err(priv->dev, "Failed to fetch SoC revision.");
		if (info_list->soc_register)
			dev_err(priv->dev, "Failed to do SoC registration.");
		err = -EINVAL;
		return err;
	}

	if (!info_list->soc_register)
		return 0;

	attr = devm_kzalloc(priv->dev, sizeof(*attr), GFP_KERNEL);
	if (!attr)
		return -ENOMEM;

	if (FIELD_GET(DEV_GETINFO_MIN_VER_MASK, var_se_info.soc_rev))
		attr->revision = devm_kasprintf(priv->dev, GFP_KERNEL, "%x.%x",
						FIELD_GET(DEV_GETINFO_MIN_VER_MASK,
							  var_se_info.soc_rev),
						FIELD_GET(DEV_GETINFO_MAJ_VER_MASK,
							  var_se_info.soc_rev));
	else
		attr->revision = devm_kasprintf(priv->dev, GFP_KERNEL, "%x",
						FIELD_GET(DEV_GETINFO_MAJ_VER_MASK,
							  var_se_info.soc_rev));

	switch (info_list->soc_id) {
	case SOC_ID_OF_IMX8ULP:
		attr->soc_id = devm_kasprintf(priv->dev, GFP_KERNEL,
					      "i.MX8ULP");
		break;
	case SOC_ID_OF_IMX93:
		attr->soc_id = devm_kasprintf(priv->dev, GFP_KERNEL,
					      "i.MX93");
		break;
	}

	err = of_property_read_string(of_root, "model",
				      &attr->machine);
	if (err)
		return -EINVAL;

	attr->family = devm_kasprintf(priv->dev, GFP_KERNEL, "Freescale i.MX");

	attr->serial_number
		= devm_kasprintf(priv->dev, GFP_KERNEL, "%016llX",
				 GET_SERIAL_NUM_FROM_UID(s_info->d_info.uid, MAX_UID_SIZE >> 2));

	sdev = soc_device_register(attr);
	if (IS_ERR(sdev))
		return PTR_ERR(sdev);

	return 0;
}

static int se_load_firmware(struct se_if_priv *priv)
{
	struct se_fw_load_info *load_fw = get_load_fw_instance(priv);
	const struct firmware *fw;
	phys_addr_t se_fw_phyaddr;
	const u8 *se_img_file_to_load;
	u8 *se_fw_buf;
	int ret;

	if (load_fw->is_fw_loaded)
		return 0;

	se_img_file_to_load = load_fw->se_fw_img_nm->seco_fw_nm_in_rfs;
	if (load_fw->se_fw_img_nm->prim_fw_nm_in_rfs &&
			load_fw->imem.state == ELE_IMEM_STATE_BAD)
		se_img_file_to_load = load_fw->se_fw_img_nm->prim_fw_nm_in_rfs;

	do {
		ret = request_firmware(&fw, se_img_file_to_load, priv->dev);
		if (ret)
			goto exit;

		dev_info(priv->dev, "loading firmware %s\n", se_img_file_to_load);

		/* allocate buffer to store the SE FW */
		se_fw_buf = dma_alloc_coherent(priv->dev, fw->size,
				&se_fw_phyaddr, GFP_KERNEL);
		if (!se_fw_buf) {
			ret = -ENOMEM;
			goto exit;
		}

		memcpy(se_fw_buf, fw->data, fw->size);
		ret = ele_fw_authenticate(priv, se_fw_phyaddr);
		if (ret < 0) {
			dev_err(priv->dev,
					"Error %pe: Authenticate & load SE firmware %s.\n",
					ERR_PTR(ret),
					se_img_file_to_load);
			ret = -EPERM;
		}

		dma_free_coherent(priv->dev,
				  fw->size,
				  se_fw_buf,
				  se_fw_phyaddr);

		release_firmware(fw);

		if (!ret && load_fw->imem.state == ELE_IMEM_STATE_BAD &&
				se_img_file_to_load == load_fw->se_fw_img_nm->prim_fw_nm_in_rfs)
			se_img_file_to_load = load_fw->se_fw_img_nm->seco_fw_nm_in_rfs;
		else
			se_img_file_to_load = NULL;

	} while (se_img_file_to_load);

	if (!ret)
		load_fw->is_fw_loaded = true;

exit:
	return ret;
}

/* interface for managed res to free a mailbox channel */
static void if_mbox_free_channel(void *mbox_chan)
{
	mbox_free_channel(mbox_chan);
}

static int se_if_request_channel(struct device *dev,
				 struct mbox_chan **chan,
				 struct mbox_client *cl,
				 const char *name)
{
	struct mbox_chan *t_chan;
	int ret = 0;

	t_chan = mbox_request_channel_byname(cl, name);
	if (IS_ERR(t_chan)) {
		ret = PTR_ERR(t_chan);
		return dev_err_probe(dev, ret,
				     "Failed to request %s channel.", name);
	}

	ret = devm_add_action(dev, if_mbox_free_channel, t_chan);
	if (ret) {
		dev_err(dev, "failed to add devm removal of mbox %s\n", name);
		goto exit;
	}

	*chan = t_chan;

exit:
	return ret;
}

static void se_if_probe_cleanup(void *plat_dev)
{
	struct platform_device *pdev = plat_dev;
	struct device *dev = &pdev->dev;
	struct se_fw_load_info *load_fw;
	struct se_if_priv *priv;

	priv = dev_get_drvdata(dev);
	load_fw = get_load_fw_instance(priv);

	/* In se_if_request_channel(), passed the clean-up functional
	 * pointer reference as action to devm_add_action().
	 * No need to free the mbox channels here.
	 */

	/* free the buffer in se remove, previously allocated
	 * in se probe to store encrypted IMEM
	 */
	if (load_fw && load_fw->imem.buf) {
		dmam_free_coherent(dev,
				   ELE_IMEM_SIZE,
				   load_fw->imem.buf,
				   load_fw->imem.phyaddr);
		load_fw->imem.buf = NULL;
	}

	/* No need to check, if reserved memory is allocated
	 * before calling for its release. Or clearing the
	 * un-set bit.
	 */
	of_reserved_mem_device_release(dev);
}

static int se_if_probe(struct platform_device *pdev)
{
	const struct se_if_node_info_list *info_list;
	const struct se_if_node_info *info;
	struct device *dev = &pdev->dev;
	struct se_fw_load_info *load_fw;
	struct se_if_priv *priv;
	u32 idx;
	int ret;

	idx = GET_IDX_FROM_DEV_NODE_NAME(dev->of_node);
	info_list = device_get_match_data(dev);
	if (idx >= info_list->num_mu) {
		dev_err(dev,
			"Incorrect node name :%s\n",
			dev->of_node->full_name);
		dev_err(dev,
			"%s-<index>, acceptable index range is 0..%d\n",
			dev->of_node->name,
			info_list->num_mu - 1);
		ret = -EINVAL;
		return ret;
	}

	info = &info_list->info[idx];
	if (!info) {
		ret = -EINVAL;
		goto exit;
	}

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto exit;
	}

	priv->dev = dev;
	priv->if_defs = &info->if_defs;
	dev_set_drvdata(dev, priv);

	ret = devm_add_action(dev, se_if_probe_cleanup, pdev);
	if (ret)
		goto exit;


	/* Mailbox client configuration */
	priv->se_mb_cl.dev		= dev;
	priv->se_mb_cl.tx_block		= false;
	priv->se_mb_cl.knows_txdone	= true;
	priv->se_mb_cl.rx_callback	= se_if_rx_callback;

	ret = se_if_request_channel(dev, &priv->tx_chan,
			&priv->se_mb_cl, MBOX_TX_NAME);
	if (ret)
		goto exit;

	ret = se_if_request_channel(dev, &priv->rx_chan,
			&priv->se_mb_cl, MBOX_RX_NAME);
	if (ret)
		goto exit;

	mutex_init(&priv->se_if_cmd_lock);

	init_completion(&priv->waiting_rsp_clbk_hdl.done);
	init_completion(&priv->cmd_receiver_clbk_hdl.done);

	if (info->pool_name) {
		priv->mem_pool = of_gen_pool_get(dev->of_node,
							 info->pool_name, 0);
		if (!priv->mem_pool) {
			dev_err(dev,
				"Unable to get sram pool = %s\n",
				info->pool_name);
			goto exit;
		}
	}

	if (info->reserved_dma_ranges) {
		ret = of_reserved_mem_device_init(dev);
		if (ret) {
			dev_err(dev,
				"failed to init reserved memory region %d\n",
				ret);
			goto exit;
		}
	}

	if (info->if_defs.se_if_type == SE_TYPE_ID_HSM) {
		ret = se_soc_info(priv);
		if (ret) {
			dev_err(dev,
				"failed[%pe] to fetch SoC Info\n", ERR_PTR(ret));
			goto exit;
		}
	}

	/* By default, there is no pending FW to be loaded.*/
	if (info_list->se_fw_img_nm.prim_fw_nm_in_rfs ||
			info_list->se_fw_img_nm.seco_fw_nm_in_rfs) {
		load_fw = get_load_fw_instance(priv);
		load_fw->se_fw_img_nm = &info_list->se_fw_img_nm;
		load_fw->is_fw_loaded = false;

		if (info_list->se_fw_img_nm.prim_fw_nm_in_rfs) {
			/* allocate buffer where SE store encrypted IMEM */
			load_fw->imem.buf = dmam_alloc_coherent(priv->dev, ELE_IMEM_SIZE,
								&load_fw->imem.phyaddr,
								GFP_KERNEL);
			if (!load_fw->imem.buf) {
				dev_err(priv->dev,
					"dmam-alloc-failed: To store encr-IMEM.\n");
				ret = -ENOMEM;
				goto exit;
			}
			load_fw->imem_mgmt = true;
		}
	}
	dev_info(dev, "i.MX secure-enclave: %s%d interface to firmware, configured.\n",
			SE_TYPE_STR_HSM,
			priv->if_defs->se_instance_id);
	return ret;

exit:
	/* if execution control reaches here, if probe fails.
	 */
	return dev_err_probe(dev, ret, "%s: Probe failed.", __func__);
}

static void se_if_remove(struct platform_device *pdev)
{
	se_if_probe_cleanup(pdev);
}

static int se_suspend(struct device *dev)
{
	struct se_if_priv *priv = dev_get_drvdata(dev);
	struct se_fw_load_info *load_fw;
	int ret = 0;

	load_fw = get_load_fw_instance(priv);

	if (load_fw->imem_mgmt)
		ret = se_save_imem_state(priv, &load_fw->imem);

	return ret;
}

static int se_resume(struct device *dev)
{
	struct se_if_priv *priv = dev_get_drvdata(dev);
	struct se_fw_load_info *load_fw;

	load_fw = get_load_fw_instance(priv);

	if (load_fw->imem_mgmt)
		se_restore_imem_state(priv, &load_fw->imem);

	return 0;
}

static const struct dev_pm_ops se_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(se_suspend, se_resume)
};

static struct platform_driver se_driver = {
	.driver = {
		.name = "fsl-se",
		.of_match_table = se_match,
		.pm = &se_pm,
	},
	.probe = se_if_probe,
	.remove = se_if_remove,
};
MODULE_DEVICE_TABLE(of, se_match);

module_platform_driver(se_driver);
MODULE_AUTHOR("Pankaj Gupta <pankaj.gupta@nxp.com>");
MODULE_DESCRIPTION("iMX Secure Enclave Driver.");
MODULE_LICENSE("GPL");
