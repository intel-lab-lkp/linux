// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Copyright (c) 2025 MediaTek Inc.
 * Author: Yunfei Dong <yunfei.dong@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#if IS_ENABLED(CONFIG_MTK_VCP_RPROC)
#include <linux/remoteproc.h>
#include <linux/remoteproc/mtk_vcp_public.h>
#endif

#define mtk_vmm_dbg(dev, fmt, args...) \
	dev_dbg(dev, "[vmm] %s(%d): " fmt "\n", __func__, __LINE__, ##args)

#define mtk_vmm_err(dev, fmt, args...) \
	dev_err(dev, "[vmm] err %s(%d): " fmt "\n", __func__, __LINE__, ##args)

/* vdisp' id=0, vdec' id=1 */
#define HW_CCF_XPU0_BACKUP_SET(id)      (0x230 + (id) * 0x8)
#define HW_CCF_XPU0_BACKUP_CLR(id)      (0x234 + (id) * 0x8)
#define HW_CCF_BACKUP_ENABLE(id)        (0x1430 + (id) * 0x10)
#define HW_CCF_BACKUP_STATUS(id)        (0x1434 + (id) * 0x10)
#define HW_CCF_BACKUP_STATUS_DBG(id)    (0x1438 + (id) * 0x10)
#define HW_CCF_BACKUP_DONE(id)          (0x143c + (id) * 0x10)
#define HW_CCF_BACKUP_SET_STATUS(id)    (0x1484 + (id) * 0x8)
#define HW_CCF_BACKUP_CLR_STATUS(id)    (0x1488 + (id) * 0x8)

#define DELAY_US      (1)
#define POLL_TIMEOUT  (10000)

#define MTK_VMM_REGULAR(match, _name)                  \
[MTK_VMM_ID_##_name] = {                               \
	.desc = {                                      \
		.name = match,                         \
		.of_match = of_match_ptr(match),       \
		.ops = &mtk_vmm_regulator_ops,         \
		.type = REGULATOR_VOLTAGE,             \
		.id = MTK_VMM_ID_##_name,              \
		.owner = THIS_MODULE,                  \
	},                                             \
}

/*
 * enum mtk_vmm_id - mtk vmm id' information
 *
 * @MTK_VMM_ID_VDISP: vdisp BIT, id
 * @MTK_VMM_ID_VDEC_VCORE: vdec BIT, id
 * @MTK_VMM_ID_MAX: invalid id
 */
enum mtk_vmm_id {
	MTK_VMM_ID_VDISP = 0,
	MTK_VMM_ID_VDEC_VCORE,
	MTK_VMM_ID_MAX
};

/*
 * struct mtk_vmm_drv - mtk vmm driver' information
 *
 * @vcp_device: vcp device
 * @vcp_nb: vcp notifier block
 * @hwccf_regmap: hardware common clock framework register map
 * @dev: vmm device
 * @kthr_vcp: vcp task thread
 * @vcp_phandle: vcp handle
 * @vcp_is_active: vcp active status
 * @need_update: vcore regulator enable and disable
 * @disable_status: disable status.
 */
struct mtk_vmm_drv {
	struct mtk_vcp_device  *vcp_device;
	struct notifier_block  vcp_nb;
	struct regmap          *hwccf_regmap;
	/* lock for hwccf control */
	struct mutex           ctrl_mutex[MTK_VMM_ID_MAX];
	struct device          *dev;
	struct task_struct     *kthr_vcp;
	phandle                vcp_phandle;
	bool                   vcp_is_active;
	bool                   need_update[MTK_VMM_ID_MAX];
	bool                   disable_status[MTK_VMM_ID_MAX];
};

/*
 * struct mtk_vmm_regulator - mtk vmm regulators' information
 *
 * @desc: standard fields of regulator description.
 */
struct mtk_vmm_regulator {
	struct regulator_desc	desc;
};

/*
 * struct mtk_vmm_regulator_init_data - mtk vmm regulators' init data
 *
 * @size: num of regulators
 * @regulator_info: regulator info.
 */
struct mtk_vmm_regulator_init_data {
	u32 size;
	struct mtk_vmm_regulator *regulator_info;
};

static void mtk_vmm_debug_dump(struct mtk_vmm_drv *vmm_drv, int line, int id)
{
	u32 value0, value1, value2, value3, value4, value5, value6, value7;
	u32 ccf_set;
	u32 ccf_clr;
	u32 ccf_enable;
	u32 ccf_status;
	u32 ccf_status_dbg;
	u32 ccf_done;
	u32 ccf_set_status;
	u32 ccf_clr_status;

	ccf_set = HW_CCF_XPU0_BACKUP_SET(id);
	ccf_clr = HW_CCF_XPU0_BACKUP_CLR(id);
	ccf_enable = HW_CCF_BACKUP_ENABLE(id);
	ccf_status = HW_CCF_BACKUP_STATUS(id);
	ccf_status_dbg = HW_CCF_BACKUP_STATUS_DBG(id);
	ccf_done = HW_CCF_BACKUP_DONE(id);
	ccf_set_status = HW_CCF_BACKUP_SET_STATUS(id);
	ccf_clr_status = HW_CCF_BACKUP_CLR_STATUS(id);

	regmap_read(vmm_drv->hwccf_regmap, ccf_set, &value0);
	regmap_read(vmm_drv->hwccf_regmap, ccf_clr, &value1);
	regmap_read(vmm_drv->hwccf_regmap, ccf_enable, &value2);
	regmap_read(vmm_drv->hwccf_regmap, ccf_status, &value3);
	regmap_read(vmm_drv->hwccf_regmap, ccf_status_dbg, &value4);
	regmap_read(vmm_drv->hwccf_regmap, ccf_done, &value5);
	regmap_read(vmm_drv->hwccf_regmap, ccf_set_status, &value6);
	regmap_read(vmm_drv->hwccf_regmap, ccf_clr_status, &value7);

	mtk_vmm_err(vmm_drv->dev, "id:%d %d: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",
		    id, line, value0, value1, value2,
		    value3, value4, value5, value6, value7);
}

static int mtk_vmm_hwccf_ctrl(struct mtk_vmm_drv *vmm_drv, bool enable, int id)
{
	u32 ctrl_reg;
	u32 hwccf_ctrl_status;
	u32 hwccf_done;
	u32 bit_val;
	int tmp, ret = 0;

	if (IS_ERR_OR_NULL(vmm_drv->hwccf_regmap)) {
		mtk_vmm_err(vmm_drv->dev, "hwccf_reg_base is null, need to map first.");
		return -ENODEV;
	}

	if (!vmm_drv->vcp_is_active) {
		vmm_drv->need_update[id] = true;
		vmm_drv->disable_status[id] = !enable;
		goto ctrl_end;
	}

	ctrl_reg = enable ? HW_CCF_XPU0_BACKUP_SET(id) : HW_CCF_XPU0_BACKUP_CLR(id);
	hwccf_ctrl_status = enable ? HW_CCF_BACKUP_SET_STATUS(id) : HW_CCF_BACKUP_CLR_STATUS(id);
	hwccf_done = HW_CCF_BACKUP_DONE(id);
	bit_val = BIT(id);

	/* polling hwccf write data done */
	ret = regmap_read_poll_timeout(vmm_drv->hwccf_regmap, hwccf_done, tmp,
				       ((tmp & bit_val) == bit_val), DELAY_US, POLL_TIMEOUT);
	if (ret < 0) {
		mtk_vmm_debug_dump(vmm_drv, __LINE__, id);
		goto ctrl_end;
	}

	/* set and clr data */
	ret = regmap_write(vmm_drv->hwccf_regmap, ctrl_reg, bit_val);
	if (ret) {
		mtk_vmm_debug_dump(vmm_drv, __LINE__, id);
		goto ctrl_end;
	}

	/* polling hwccf write data done */
	ret = regmap_read_poll_timeout(vmm_drv->hwccf_regmap, hwccf_done, tmp,
				       ((tmp & bit_val) == bit_val), DELAY_US, POLL_TIMEOUT);
	if (ret < 0) {
		mtk_vmm_debug_dump(vmm_drv, __LINE__, id);
		goto ctrl_end;
	}

	/* wait for current done */
	ret = regmap_read_poll_timeout(vmm_drv->hwccf_regmap, hwccf_ctrl_status, tmp,
				       !(tmp & bit_val), DELAY_US, POLL_TIMEOUT);
	if (ret < 0) {
		mtk_vmm_debug_dump(vmm_drv, __LINE__, id);
		goto ctrl_end;
	}
ctrl_end:

	return ret;
}

static int mtk_vmm_is_enabled(struct regulator_dev *rdev)
{
	u32 hwccf_enable;
	u32 bit_val;
	struct mtk_vmm_drv *vmm_drv = rdev_get_drvdata(rdev);
	int ret = 0;
	u32 val;
	int id = rdev->desc->id;

	if (id >= MTK_VMM_ID_MAX) {
		mtk_vmm_err(vmm_drv->dev, "desc id is error %d\n", __LINE__);
		goto ctrl_end;
	}

	hwccf_enable = HW_CCF_BACKUP_ENABLE(id);
	bit_val = BIT(id);

	ret = regmap_read(vmm_drv->hwccf_regmap, hwccf_enable, &val);
	if (ret < 0)
		mtk_vmm_err(vmm_drv->dev, "vmm id:%d read buck status failed\n", id);
	else
		ret = !!(val & bit_val);
ctrl_end:

	return ret;
}

static int mtk_vmm_enable(struct regulator_dev *rdev)
{
	struct mtk_vmm_drv *vmm_drv = rdev_get_drvdata(rdev);
	int ret;
	int id = rdev->desc->id;

	mtk_vmm_dbg(vmm_drv->dev, "id:%d enable vmm driver %d",
		    id, vmm_drv->vcp_is_active);
	if (id >= MTK_VMM_ID_MAX) {
		mtk_vmm_err(vmm_drv->dev, "desc id is error %d\n", __LINE__);
		return -ENODEV;
	}

	mutex_lock(&vmm_drv->ctrl_mutex[id]);
	ret = mtk_vmm_hwccf_ctrl(vmm_drv, true, id);
	if (ret < 0)
		mtk_vmm_err(vmm_drv->dev, "failed to enable hwccf, ret=%d\n", ret);
	mutex_unlock(&vmm_drv->ctrl_mutex[id]);

	return ret;
}

static int mtk_vmm_disable(struct regulator_dev *rdev)
{
	struct mtk_vmm_drv *vmm_drv = rdev_get_drvdata(rdev);
	int ret = 0;
	int id = rdev->desc->id;

	mtk_vmm_dbg(vmm_drv->dev, "id:%d disable vmm driver %d",
		    id, vmm_drv->vcp_is_active);
	if (id >= MTK_VMM_ID_MAX) {
		mtk_vmm_err(vmm_drv->dev, "desc id is error %d\n", __LINE__);
		return -ENODEV;
	}

	mutex_lock(&vmm_drv->ctrl_mutex[id]);
	ret = mtk_vmm_hwccf_ctrl(vmm_drv, false, id);
	if (ret < 0)
		mtk_vmm_err(vmm_drv->dev, "failed to disable hwccf, ret=%d\n", ret);
	mutex_unlock(&vmm_drv->ctrl_mutex[id]);

	return ret;
}

#if IS_ENABLED(CONFIG_MTK_VCP_RPROC)
static int vmm_vcp_notifier(struct notifier_block *nb, unsigned long vcp_event, void *unused)
{
	struct mtk_vmm_drv *vmm_drv = container_of(nb, struct mtk_vmm_drv, vcp_nb);
	int ret = NOTIFY_DONE;
	int id;

	switch (vcp_event) {
	case VCP_EVENT_SUSPEND:
	case VCP_EVENT_STOP:
		mtk_vmm_dbg(vmm_drv->dev, "vcp notifier suspend");
		break;
	case VCP_EVENT_READY:
	case VCP_EVENT_RESUME:
		for (id = MTK_VMM_ID_VDISP; id < MTK_VMM_ID_MAX; id++) {
			mutex_lock(&vmm_drv->ctrl_mutex[id]);
			if (vmm_drv->need_update[id]) {
				ret = mtk_vmm_hwccf_ctrl(vmm_drv, true, id);
				if (ret < 0)
					mtk_vmm_err(vmm_drv->dev,
						    "failed to enable hwccf %d\n", ret);

				if (vmm_drv->disable_status[id]) {
					ret = mtk_vmm_hwccf_ctrl(vmm_drv, false, id);
					if (ret < 0)
						mtk_vmm_err(vmm_drv->dev,
							    "failed to disable hwccf %d\n",
							    ret);
					vmm_drv->disable_status[id] = false;
				}

				vmm_drv->need_update[id] = false;
			}
			mutex_unlock(&vmm_drv->ctrl_mutex[id]);
		}

		mtk_vmm_dbg(vmm_drv->dev, "vcp notifier ready");
		break;
	}

	return ret;
}

static int mtk_vmm_vcp_init_thread(void *arg)
{
	struct mtk_vmm_drv *vmm_drv = arg;
	struct device *dev = vmm_drv->dev;
	struct device_link *dev_link;
	int retry = 0, retry_cnt = 10000;

	while (request_module("mtk-vcp")) {
		if (++retry > retry_cnt) {
			mtk_vmm_err(vmm_drv->dev, "failed to load mtk-vcp module");
			return -ENODEV;
		}
		ssleep(1);
	}

	if (of_property_read_u32(dev->of_node, "mediatek,vcp", &vmm_drv->vcp_phandle)) {
		mtk_vmm_err(vmm_drv->dev, "can't get vcp handle.\n");
		return -ENODEV;
	}

	vmm_drv->vcp_device = mtk_vcp_get_by_phandle(vmm_drv->vcp_phandle);
	while (!vmm_drv->vcp_device) {
		mtk_vmm_err(vmm_drv->dev, "get vcp device failed\n");
		return -ENODEV;
	}

	dev_link = device_link_add(vmm_drv->dev, vmm_drv->vcp_device->dev, 0);
	if (!dev_link) {
		mtk_vmm_err(vmm_drv->dev, "device link is NULL\n");
		return -EINVAL;
	}

	retry = 0;
	retry_cnt = 10000;
	while (!vmm_drv->vcp_device->data->vcp_is_ready(MMDVFS_MMUP_FEATURE_ID) ||
	       !vmm_drv->vcp_device->data->vcp_is_ready(MMDVFS_VCP_FEATURE_ID)) {
		if (++retry > retry_cnt) {
			mtk_vmm_err(vmm_drv->dev, "vcp and mmup is not ready yet.");
			return -ETIMEDOUT;
		}
		ssleep(1);
	}

	vmm_drv->vcp_is_active = true;
	vmm_drv->vcp_device->data->vcp_register_feature(vmm_drv->vcp_device, VMM_FEATURE_ID);

	vmm_drv->vcp_nb.notifier_call = vmm_vcp_notifier;
	vmm_drv->vcp_device->data->vcp_register_notify(VMM_FEATURE_ID, &vmm_drv->vcp_nb);

	return 0;
}
#endif

static const struct regulator_ops mtk_vmm_regulator_ops = {
	.enable = mtk_vmm_enable,
	.disable = mtk_vmm_disable,
	.is_enabled = mtk_vmm_is_enabled,
};

static struct mtk_vmm_regulator mt8196_vmm_regulators[] = {
	MTK_VMM_REGULAR("vdisp", VDISP),
	MTK_VMM_REGULAR("vdec-vcore", VDEC_VCORE),
};

static const struct mtk_vmm_regulator_init_data mt8196_regulator_data = {
	.size = ARRAY_SIZE(mt8196_vmm_regulators),
	.regulator_info = mt8196_vmm_regulators,
};

static const struct of_device_id of_vmm_match_tbl[] = {
	{
		.compatible = "mediatek,mt8196-vmm",
		.data = &mt8196_regulator_data,
	},
	{},
};
MODULE_DEVICE_TABLE(of, of_vmm_match_tbl);

static int mtk_vmm_probe(struct platform_device *pdev)
{
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	const struct mtk_vmm_regulator_init_data *regulator_init_data;
	struct mtk_vmm_regulator *mtk_regulators;
	struct mtk_vmm_drv *vmm_drv;
	int i;

	vmm_drv = devm_kzalloc(&pdev->dev, sizeof(*vmm_drv), GFP_KERNEL);
	if (!vmm_drv)
		return -ENOMEM;

	vmm_drv->dev = &pdev->dev;
	for (i = MTK_VMM_ID_VDISP; i < MTK_VMM_ID_MAX; i++)
		mutex_init(&vmm_drv->ctrl_mutex[i]);

	platform_set_drvdata(pdev, vmm_drv);

	vmm_drv->hwccf_regmap =
		syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "mediatek,hw-ccf");
	if (IS_ERR(vmm_drv->hwccf_regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(vmm_drv->hwccf_regmap),
				     "cannot find hwccf controller.\n");

	regulator_init_data = of_device_get_match_data(&pdev->dev);

	mtk_regulators = regulator_init_data->regulator_info;
	for (i = 0; i < regulator_init_data->size; i++) {
		config.dev = &pdev->dev;
		config.driver_data = vmm_drv;
		rdev = devm_regulator_register(&pdev->dev, &(mtk_regulators + i)->desc, &config);
		if (IS_ERR(rdev)) {
			dev_err(&pdev->dev, "failed to register %d\n", i);
			return PTR_ERR(rdev);
		}
	}

#if IS_ENABLED(CONFIG_MTK_VCP_RPROC)
	vmm_drv->kthr_vcp = kthread_run(mtk_vmm_vcp_init_thread, vmm_drv, "vmm-vcp");
	if (IS_ERR(vmm_drv->kthr_vcp))
		dev_err(&pdev->dev, "create kthread failed");
#endif

	mtk_vmm_dbg(&pdev->dev, "vmm prob done.");
	return 0;
}

static void mtk_vmm_remove(struct platform_device *pdev)
{
#if IS_ENABLED(CONFIG_MTK_VCP_RPROC)
	struct mtk_vmm_drv *vmm_drv = dev_get_drvdata(&pdev->dev);
	struct mtk_vcp_device *vcp_device = vmm_drv->vcp_device;

	vcp_device->data->vcp_deregister_feature(vcp_device, VMM_FEATURE_ID);
	device_link_remove(vmm_drv->dev, vcp_device->dev);
#endif
}

static struct platform_driver mtk_vmm_plat_drv = {
	.probe = mtk_vmm_probe,
	.remove = mtk_vmm_remove,
	.driver = {
		.name = "mtk-vmm-drv",
		.of_match_table = of_vmm_match_tbl,
	},
};
module_platform_driver(mtk_vmm_plat_drv);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek vmm driver");
