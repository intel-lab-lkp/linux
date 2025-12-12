// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPIO driver for Advantech EIO embedded controller.
 *
 * Copyright (C) 2025 Advantech Corporation. All rights reserved.
 */

#include <linux/errno.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/mfd/core.h>
#include <linux/mfd/eio.h>
#include <linux/module.h>

#define MAX_DEV 128
#define MAX_NAME 32

static uint timeout;
module_param(timeout, uint, 0444);
MODULE_PARM_DESC(timeout,
		 "Default pmc command timeout in micro-seconds.\n");

struct eio_hwmon_dev {
	struct device *mfd;
};

enum _sen_type {
	NONE,
	VOLTAGE,
	CURRENT,
	TEMP,
	PWM,
	TACHO,
	FAN,
	CASEOPEN,
};

struct eio_key {
	enum _sen_type type;
	u8 chan;
	u8 item;
	u8 label_idx;
};

struct eio_attr {
	struct sensor_device_attribute sda;
	struct eio_key key;
};

static struct {
	u8   cmd;
	u8   max;
	signed int shift;
	char name[32];
	u8   ctrl[16];
	u16  multi[16];
	char item[16][32];
	char labels[32][32];

} sen_info[] = {
	{ 0x00, 0, 0, "none" },
	{ 0x12, 8, 0, "in",
		{ 0xFF, 0x10, 0x11, 0x12 },
		{ 1,    10,   10,   10 },
		{ "label", "input", "max", "min" },
		{ "5V", "5Vs5", "12V", "12Vs5",
		  "3V3", "3V3", "5Vsb", "3Vsb",
		  "Vcmos", "Vbat", "Vdc", "Vstb",
		  "Vcore_a", "Vcore_b", "", "",
		  "Voem0", "Voem1", "Voem2", "Voem3"
		},
	},
	{ 0x1a, 2, 0, "curr",
		{ 0xFF, 0x10, 0x11, 0x12 },
		{ 1,    10,   10,   10 },
		{ "label", "input", "max", "min" },
		{ "dc", "oem0" },
	},
	{ 0x10, 4, -2731, "temp",
		{ 0xFF, 0x10, 0x11, 0x12, 0x21, 0x41 },
		{ 1,    100,  100,  100,  100,  100 },
		{ "label", "input", "max", "min", "crit", "emergency" },
		{ "cpu0", "cpu1", "cpu2", "cpu3",
		  "sys0", "sys1", "sys2", "sys3",
		  "aux0", "aux1", "aux2", "aux3",
		  "dimm0", "dimm1", "dimm2", "dimm3",
		  "pch", "gpu", "", "",
		  "", "", "", "",
		  "", "", "", "",
		  "oem0", "oem1", "oem", "oem3" },
	},
	{ 0x14, 0, 0, "pwm",
		{ 0xFF, 0x11, 0x12 },
		{ 1, 1, 1 },
		{ "label", "polarity", "freq" },
		{ "pwm0", "pwm0", "pwm0", "pwm0" },
	},
	{ 0x16, 2, 0, "tacho",
		{ 0xFF, 0x10 },
		{ 1, 1 },
		{ "label", "input"},
		{ "cpu0", "cpu1", "cpu2", "cpu3",
		  "sys0", "sys1", "sys2", "sys3",
		  "", "", "", "", "", "", "", "",
		  "", "", "", "", "", "", "", "",
		  "", "", "", "",
		  "oem0", "oem1", "oem2", "oem3"
		},
	},
	{ 0x24, 4, 0, "fan",
		{ 0xFF, 0x1A },
		{ 1, 1 },
		{ "label", "input"},
		{ "cpu0", "cpu1", "cpu2", "cpu3",
		  "sys0", "sys1", "sys2", "sys3",
		  "", "", "", "", "", "", "", "",
		  "", "", "", "", "", "", "", "",
		  "", "", "", "",
		  "oem0", "oem1", "oem2", "oem3",
		},
	},
	{ 0x28, 1, 0, "intrusion",
		{ 0xFF, 0x02 },
		{ 1, 1 },
		{ "label", "input" },
		{ "case_open" }
	}
};

static struct {
	enum _sen_type type;
	u8   ctrl;
	int  size;
	bool write;

} ctrl_para[] = {
	{ NONE,     0x00, 0, false },

	{ VOLTAGE,  0x00, 1, false }, { VOLTAGE,  0x01, 1, false },
	{ VOLTAGE,  0x10, 2, false }, { VOLTAGE,  0x11, 2, false },
	{ VOLTAGE,  0x12, 2, false },

	{ CURRENT,  0x00, 1, false }, { CURRENT,  0x01, 1, false },
	{ CURRENT,  0x10, 2, false }, { CURRENT,  0x11, 2, false },
	{ CURRENT,  0x12, 2, false },

	{ TEMP,	    0x00, 2, false }, { TEMP,	  0x01, 1, false },
	{ TEMP,     0x04, 1, false }, { TEMP,	  0x10, 2, false },
	{ TEMP,     0x11, 2, false }, { TEMP,	  0x12, 2, false },
	{ TEMP,     0x21, 2, false }, { TEMP,	  0x41, 2, false },

	{ PWM,      0x00, 1, false }, { PWM,	  0x10, 1, true  },
	{ PWM,      0x11, 1, true  }, { PWM,	  0x12, 4, true  },

	{ TACHO,    0x00, 1, false }, { TACHO,	  0x01, 1, false },
	{ TACHO,    0x10, 4, true  },

	{ FAN,      0x00, 1, false }, { FAN,	  0x01, 1, false },
	{ FAN,      0x03, 1, true  }, { FAN,	  0x1A, 2, false },

	{ CASEOPEN, 0x00, 1, false }, { CASEOPEN, 0x02, 1, true  },
};

static int para_idx(enum _sen_type type, u8 ctrl)
{
	int i;

	for (i = 1 ; i < ARRAY_SIZE(ctrl_para) ; i++)
		if (type == ctrl_para[i].type &&
		    ctrl == ctrl_para[i].ctrl)
			return i;

	return 0;
}

static int pmc_read(struct device *mfd, enum _sen_type type, u8 dev_id, u8 ctrl, void *data)
{
	int idx = para_idx(type, ctrl);
	int ret = 0;

	if (idx == 0)
		return -EINVAL;

	if (WARN_ON(!data))
		return -EINVAL;

	struct pmc_op op = {
		 .cmd       = sen_info[type].cmd | EIO_FLAG_PMC_READ,
		 .control   = ctrl,
		 .device_id = dev_id,
		 .size	    = ctrl_para[idx].size,
		 .payload   = (u8 *)data,
		 .timeout   = timeout,
	};

	ret = eio_core_pmc_operation(mfd, &op);
	return ret;
}

static ssize_t eio_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct eio_hwmon_dev *eio_hwmon = dev_get_drvdata(dev);
	struct eio_attr *eio_attr =
		container_of(attr, struct eio_attr, sda.dev_attr);
	const struct eio_key *eio_key = &eio_attr->key;
	int ret;
	u8 data[2];
	u32 temp_val;
	signed int final_val;

	switch (eio_key->item) {
	case 0:
		return sysfs_emit(buf, "%s\n",
				  sen_info[eio_key->type].labels[eio_key->label_idx]);

	default:
		ret = pmc_read(eio_hwmon->mfd, eio_key->type, eio_key->chan,
			       sen_info[eio_key->type].ctrl[eio_key->item],
			       &data);
		if (ret)
			return ret;

		temp_val = data[0] | data[1] << 8;

		final_val = (signed int)temp_val + (signed int)(sen_info[eio_key->type].shift);
		final_val = final_val * (signed int)sen_info[eio_key->type].multi[eio_key->item];

		return sysfs_emit(buf, "%d\n", final_val);
	}

	return -EINVAL;
}

static char devname[MAX_DEV][MAX_NAME];
static struct eio_attr devattrs[MAX_DEV];
static struct attribute *attrs[MAX_DEV];

static struct attribute_group group = {
	.attrs = attrs,
};

static const struct attribute_group *groups[] = {
	&group,
	NULL
};

static int hwmon_init(struct device *mfd, struct eio_hwmon_dev *eio_hwmon)
{
	enum _sen_type type;
	u8 i, j, data[16];
	int sum = 0;
	int ret;

	for (type = VOLTAGE ; type <= CASEOPEN ; type++) {
		int cnt = 1;

		for (i = 0 ; i < sen_info[type].max ; i++) {
			if (pmc_read(mfd, type, i, 0x00, data) ||
			    (data[0] & 0x01) == 0)
				continue;

			memset(data, 0, sizeof(data));
			ret = pmc_read(mfd, type, i, 0x01, data);
			if (ret != 0 && ret != -EINVAL) {
				dev_info(mfd, "read type id error\n");
				continue;
			}

			for (j = 0 ; j < ARRAY_SIZE(sen_info->item) ; j++) {
				struct eio_attr *eio_attr;

				if (sen_info[type].item[j][0] == 0)
					continue;

				eio_attr = &devattrs[sum];

				eio_attr->key.type      = type;
				eio_attr->key.chan      = i;
				eio_attr->key.item      = j;
				eio_attr->key.label_idx = data[0];

				snprintf(devname[sum], sizeof(devname[sum]),
					 "%s%d_%s", sen_info[type].name, cnt,
					 sen_info[type].item[j]);

				eio_attr->sda.dev_attr.attr.name = devname[sum];
				eio_attr->sda.dev_attr.attr.mode = 0444;
				eio_attr->sda.dev_attr.show      = eio_show;

				attrs[sum] = &eio_attr->sda.dev_attr.attr;

				if (++sum >= MAX_DEV)
					break;
			}
			cnt++;
		}
	}

	return sum;
}

static int hwmon_probe(struct platform_device *pdev)
{
	struct device *dev =  &pdev->dev;
	struct eio_hwmon_dev *eio_hwmon;
	struct eio_dev *eio_dev = dev_get_drvdata(dev->parent);
	struct device *hwmon;

	if (!eio_dev) {
		dev_err(dev, "Error contact eio_core\n");
		return -ENODEV;
	}

	eio_hwmon = devm_kzalloc(dev, sizeof(*eio_hwmon), GFP_KERNEL);
	if (!eio_hwmon)
		return -ENOMEM;

	eio_hwmon->mfd = dev->parent;
	platform_set_drvdata(pdev, eio_hwmon);

	if (hwmon_init(dev->parent, eio_hwmon) <= 0)
		return -ENODEV;

	hwmon =	devm_hwmon_device_register_with_groups(dev, KBUILD_MODNAME,
						       eio_hwmon,
						       groups);
	return PTR_ERR_OR_ZERO(hwmon);
}

static struct platform_driver eio_hwmon_driver = {
	.probe  = hwmon_probe,
	.driver = {
		.name = "eio_hwmon",
	},
};

module_platform_driver(eio_hwmon_driver);

MODULE_AUTHOR("Wenkai Chung <wenkai.chung@advantech.com.tw>");
MODULE_AUTHOR("Ramiro Oliveira <ramiro.oliveira@advantech.com>");
MODULE_DESCRIPTION("Hardware monitor driver for Advantech EIO embedded controller");
MODULE_LICENSE("GPL");

