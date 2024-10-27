// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2024 NXP
 */

#include <linux/arm-smccc.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/sys_soc.h>

#define IMX_SIP_GET_SOC_INFO	0xc2000006
#define SOC_ID(x)		(((x) & 0xFFFF) >> 8)
#define SOC_REV_MAJOR(x)	((((x) >> 28) & 0xF) - 0x9)
#define SOC_REV_MINOR(x)	(((x) >> 24) & 0xF)

static int imx9_soc_device_register(void)
{
	struct soc_device_attribute *attr;
	struct arm_smccc_res res;
	struct soc_device *sdev;
	u32 soc_id, rev_major, rev_minor;
	u64 uid127_64, uid63_0;
	int err;

	attr = kzalloc(sizeof(*attr), GFP_KERNEL);
	if (!attr)
		return -ENOMEM;

	err = of_property_read_string(of_root, "model", &attr->machine);
	if (err) {
		err = -EINVAL;
		goto attr;
	}

	attr->family = kasprintf(GFP_KERNEL, "Freescale i.MX");

	/*
	 * Retrieve the soc id, rev & uid info:
	 * res.a1[31:16]: soc revision;
	 * res.a1[15:0]: soc id;
	 * res.a2: uid[127:64];
	 * res.a3: uid[63:0];
	 */
	arm_smccc_smc(IMX_SIP_GET_SOC_INFO, 0, 0, 0, 0, 0, 0, 0, &res);
	if (res.a0 != SMCCC_RET_SUCCESS) {
		err = -EINVAL;
		goto family;
	}

	soc_id = SOC_ID(res.a1);
	rev_major = SOC_REV_MAJOR(res.a1);
	rev_minor = SOC_REV_MINOR(res.a1);

	attr->soc_id = kasprintf(GFP_KERNEL, "i.MX%2x", soc_id);
	attr->revision = kasprintf(GFP_KERNEL, "%d.%d", rev_major, rev_minor);

	uid127_64 = res.a2;
	uid63_0 = res.a3;
	attr->serial_number = kasprintf(GFP_KERNEL, "%016llx%016llx", uid127_64, uid63_0);

	sdev = soc_device_register(attr);
	if (IS_ERR(sdev)) {
		err = -ENODEV;
		goto soc_id;
	}

	return 0;

soc_id:
	kfree(attr->soc_id);
	kfree(attr->serial_number);
	kfree(attr->revision);
family:
	kfree(attr->family);
attr:
	kfree(attr);
	return err;
}

static int __init imx9_soc_init(void)
{
	int ret = 0;

	if (of_machine_is_compatible("fsl,imx91") ||
		of_machine_is_compatible("fsl,imx93") ||
		of_machine_is_compatible("fsl,imx95")) {
		ret = imx9_soc_device_register();
		if (ret) {
			pr_err("%s: imx9_soc_device_register returned %d\n", __func__, ret);
			return ret;
		}
	}

	return ret;
}
device_initcall(imx9_soc_init);

MODULE_AUTHOR("NXP");
MODULE_DESCRIPTION("NXP i.MX9 SoC");
MODULE_LICENSE("GPL");
