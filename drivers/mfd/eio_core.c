// SPDX-License-Identifier: GPL-2.0-only
/*
 * Advantech Embedded Controller base Driver
 *
 * This driver provides an interface to access the EIO Series EC
 * firmware via its own Power Management Channel (PMC) for subdrivers:
 *
 * A system may have one or two independent EIO devices.
 *
 * Copyright (C) 2025 Advantech Co., Ltd.
 */

#include <linux/delay.h>
#include <linux/isa.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/sysfs.h>
#include <linux/time.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/mfd/eio.h>

#define TIMEOUT_MAX (10 * USEC_PER_SEC)
#define TIMEOUT_MIN 200
#define SLEEP_MAX 200
#define DEFAULT_TIMEOUT 5000

/**
 * Timeout: Default timeout in microseconds when a PMC command's
 * timeout is unspecified. PMC command responses typically range
 * from 200us to 2ms. 5ms is quite a safe value for timeout. In
 * In some cases, responses are longer. In such situations, please
 * adding the timeout parameter loading related sub-drivers or
 * this core driver (not recommended).
 */
static uint timeout = DEFAULT_TIMEOUT;
module_param(timeout, uint, 0444);
MODULE_PARM_DESC(timeout, "Default PMC command timeout in usec.\n");

struct eio_dev_port {
	u16 idx_port;
	u16 data_port;
};

static struct eio_dev_port pnp_port[] = {
	{ .idx_port = EIO_PNP_INDEX, .data_port = EIO_PNP_DATA },
	{ .idx_port = EIO_SUB_PNP_INDEX,
	  .data_port = EIO_SUB_PNP_DATA },
};

static struct mfd_cell mfd_devs[] = {
	{ .name = "eio_wdt" },
	{ .name = "gpio_eio" },
	{ .name = "eio_hwmon" },
	{ .name = "i2c_eio" },
	{ .name = "eio_thermal" },
	{ .name = "eio_fan" },
	{ .name = "eio_bl" },
};

static const struct regmap_range eio_range[] = {
	regmap_reg_range(EIO_PNP_INDEX, EIO_PNP_DATA),
	regmap_reg_range(EIO_SUB_PNP_INDEX, EIO_SUB_PNP_DATA),
	regmap_reg_range(0x200, 0x3FF),
};

static const struct regmap_access_table volatile_regs = {
	.yes_ranges = eio_range,
	.n_yes_ranges = ARRAY_SIZE(eio_range),
};

static const struct regmap_config pnp_regmap_config = {
	.name = "eio_core",
	.reg_bits = 16,
	.val_bits = 8,
	.volatile_table = &volatile_regs,
	.io_port = true,
	.cache_type = REGCACHE_NONE,
};

static struct {
	char name[32];
	int cmd;
	int ctrl;
	int dev;
	int size;
	enum {
		HEX,
		NUMBER,
		PNP_ID,
	} type;

} attrs[] = {
	{ "board_name", 0x53, 0x10, 0, 16 },
	{ "board_serial", 0x53, 0x1F, 0, 16 },
	{ "board_manufacturer", 0x53, 0x11, 0, 16 },
	{ "board_id", 0x53, 0x1E, 0, 4 },
	{ "firmware_version", 0x53, 0x21, 0, 4 },
	{ "firmware_name", 0x53, 0x22, 0, 16 },
	{ "firmware_build", 0x53, 0x23, 0, 26 },
	{ "firmware_date", 0x53, 0x24, 0, 16 },
	{ "chip_id", 0x53, 0x12, 0, 12 },
	{ "chip_detect", 0x53, 0x15, 0, 12 },
	{ "platform_type", 0x53, 0x13, 0, 16 },
	{ "platform_revision", 0x53, 0x04, 0x44, 4 },
	{ "eapi_version", 0x53, 0x04, 0x64, 4 },
	{ "eapi_id", 0x53, 0x31, 0, 4 },
	{ "boot_count", 0x55, 0x10, 0, 4, NUMBER },
	{ "powerup_hour", 0x55, 0x11, 0, 4, NUMBER },
	{ "pnp_id", 0x53, 0x04, 0x68, 4, PNP_ID },
};

static ssize_t info_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	uint i;

	for (i = 0; i < ARRAY_SIZE(attrs); i++) {
		int ret;
		char str[32] = "";
		int val;

		struct pmc_op op = {
			.cmd = attrs[i].cmd,
			.control = attrs[i].ctrl,
			.device_id = attrs[i].dev,
			.payload = (u8 *)str,
			.size = attrs[i].size,
		};

		if (strcmp(attr->attr.name, attrs[i].name))
			continue;

		ret = eio_core_pmc_operation(dev, &op);
		if (ret)
			return ret;

		if (attrs[i].size != 4)
			return sprintf(buf, "%s\n", str);

		val = *(u32 *)str;

		if (attrs[i].type == HEX)
			return sprintf(buf, "0x%08X\n", val);

		if (attrs[i].type == NUMBER)
			return sprintf(buf, "%d\n", val);

		/* Should be pnp_id */
		return sprintf(buf, "%c%c%c, %X\n", (val >> 14 & 0x3F) + 0x40,
			       ((val >> 9 & 0x18) | (val >> 25 & 0x07)) + 0x40,
			       (val >> 20 & 0x1F) + 0x40, val & 0xFFF);
	}

	return -EINVAL;
}

#define PMC_DEVICE_ATTR_RO(_name)                                             \
	static ssize_t _name##_show(struct device *dev,                       \
				    struct device_attribute *attr, char *buf) \
	{                                                                     \
		return info_show(dev, attr, buf);                             \
	}                                                                     \
	static DEVICE_ATTR_RO(_name)

PMC_DEVICE_ATTR_RO(board_name);
PMC_DEVICE_ATTR_RO(board_serial);
PMC_DEVICE_ATTR_RO(board_manufacturer);
PMC_DEVICE_ATTR_RO(firmware_name);
PMC_DEVICE_ATTR_RO(firmware_version);
PMC_DEVICE_ATTR_RO(firmware_build);
PMC_DEVICE_ATTR_RO(firmware_date);
PMC_DEVICE_ATTR_RO(chip_id);
PMC_DEVICE_ATTR_RO(chip_detect);
PMC_DEVICE_ATTR_RO(platform_type);
PMC_DEVICE_ATTR_RO(platform_revision);
PMC_DEVICE_ATTR_RO(board_id);
PMC_DEVICE_ATTR_RO(eapi_version);
PMC_DEVICE_ATTR_RO(eapi_id);
PMC_DEVICE_ATTR_RO(boot_count);
PMC_DEVICE_ATTR_RO(powerup_hour);
PMC_DEVICE_ATTR_RO(pnp_id);

static struct attribute *pmc_attrs[] = { &dev_attr_board_name.attr,
					 &dev_attr_board_serial.attr,
					 &dev_attr_board_manufacturer.attr,
					 &dev_attr_firmware_name.attr,
					 &dev_attr_firmware_version.attr,
					 &dev_attr_firmware_build.attr,
					 &dev_attr_firmware_date.attr,
					 &dev_attr_chip_id.attr,
					 &dev_attr_chip_detect.attr,
					 &dev_attr_platform_type.attr,
					 &dev_attr_platform_revision.attr,
					 &dev_attr_board_id.attr,
					 &dev_attr_eapi_version.attr,
					 &dev_attr_eapi_id.attr,
					 &dev_attr_boot_count.attr,
					 &dev_attr_powerup_hour.attr,
					 &dev_attr_pnp_id.attr,
					 NULL };

ATTRIBUTE_GROUPS(pmc);

static unsigned int eio_pnp_read(struct device *dev,
				 struct eio_dev_port *port, u8 idx)
{
	struct eio_dev *eio = dev_get_drvdata(dev);
	unsigned int val;

	if (regmap_write(eio->map, port->idx_port, idx))
		dev_err(dev, "Error port write 0x%X\n", port->idx_port);

	if (regmap_read(eio->map, port->data_port, &val))
		dev_err(dev, "Error port read 0x%X\n", port->data_port);

	return val;
}

static void eio_pnp_write(struct device *dev, struct eio_dev_port *port,
			  u8 idx, u8 data)
{
	struct eio_dev *eio = dev_get_drvdata(dev);

	if (regmap_write(eio->map, port->idx_port, idx) ||
	    regmap_write(eio->map, port->data_port, data))
		dev_err(dev, "Error port write 0x%X %X\n", port->idx_port,
			port->data_port);
}

static void eio_pnp_enter(struct device *dev, struct eio_dev_port *port)
{
	struct eio_dev *eio = dev_get_drvdata(dev);
	/* Write 0x87 to index port twice to unlock IO port */
	if (regmap_write(eio->map, port->idx_port,
			 EIO_EXT_MODE_ENTER) ||
	    regmap_write(eio->map, port->idx_port, EIO_EXT_MODE_ENTER))
		dev_err(dev, "Error port write 0x%X\n", port->idx_port);
}

static void eio_pnp_leave(struct device *dev, struct eio_dev_port *port)
{
	struct eio_dev *eio = dev_get_drvdata(dev);
	/* Write 0xAA to index port once to lock IO port */
	if (regmap_write(eio->map, port->idx_port, EIO_EXT_MODE_EXIT))
		dev_err(dev, "Error port write 0x%X\n", port->idx_port);
}

static int pmc_write_data(struct device *dev, int id, u8 value, u16 timeout)
{
	struct eio_dev *eio = dev_get_drvdata(dev);
	int ret;

	if (WAIT_IBF(dev, id, timeout))
		return -ETIME;

	ret = regmap_write(eio->map, eio->pmc[id].data, value);
	if (ret)
		dev_err(dev, "Error PMC write %X:%X\n",
			eio->pmc[id].data, value);

	return ret;
}

static int pmc_write_cmd(struct device *dev, int id, u8 value, u16 timeout)
{
	struct eio_dev *eio = dev_get_drvdata(dev);
	int ret;

	if (WAIT_IBF(dev, id, timeout))
		return -ETIME;

	ret = regmap_write(eio->map, eio->pmc[id].cmd, value);
	if (ret)
		dev_err(dev, "Error PMC write %X:%X\n",
			eio->pmc[id].cmd, value);

	return ret;
}

static int pmc_read_data(struct device *dev, int id, u8 *value, u16 timeout)
{
	struct eio_dev *eio = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	if (WAIT_OBF(dev, id, timeout))
		return -ETIME;

	ret = regmap_read(eio->map, eio->pmc[id].data, &val);
	if (ret)
		dev_err(dev, "Error PMC read %X\n", eio->pmc[id].data);
	else
		*value = (u8)(val & 0xFF);

	return ret;
}

static int pmc_read_status(struct device *dev, int id)
{
	struct eio_dev *eio = dev_get_drvdata(dev);
	unsigned int val;

	if (regmap_read(eio->map, eio->pmc[id].status, &val)) {
		dev_err(dev, "Error PMC read %X\n",
			eio->pmc[id].status);
		return 0;
	}

	return val;
}

static void pmc_clear(struct device *dev, int id)
{
	struct eio_dev *eio = dev_get_drvdata(dev);
	unsigned int val;

	/* Check if input buffer blocked */
	if ((pmc_read_status(dev, id) & EIO_PMC_STATUS_IBF) == 0)
		return;

	/* Read out previous garbage */
	if (regmap_read(eio->map, eio->pmc[id].data, &val))
		dev_err(dev, "Error pmc clear\n");

	usleep_range(10, 100);
}

int eio_core_pmc_wait(struct device *dev, int id,
		      enum eio_pmc_wait wait, uint max_duration)
{
	struct eio_dev *eio = dev_get_drvdata(dev);
	uint val;
	int new_timeout = max_duration ? max_duration : timeout;

	if (new_timeout < TIMEOUT_MIN || new_timeout > TIMEOUT_MAX) {
		dev_err(dev,
			"Error timeout value: %dus. Timeout value should between %d and %ld\n",
			new_timeout, TIMEOUT_MIN, TIMEOUT_MAX);
		return -ETIME;
	}

	if (wait == PMC_WAIT_INPUT)
		return regmap_read_poll_timeout(eio->map, eio->pmc[id].status,
						val, (val & EIO_PMC_STATUS_IBF) == 0,
						SLEEP_MAX, new_timeout);
	return regmap_read_poll_timeout(eio->map,
					eio->pmc[id].status, val,
					(val & EIO_PMC_STATUS_OBF) != 0,
					SLEEP_MAX, new_timeout);
}
EXPORT_SYMBOL_GPL(eio_core_pmc_wait);

int eio_core_pmc_operation(struct device *dev, struct pmc_op *op)
{
	struct eio_dev *eio = dev_get_drvdata(dev);
	u8 i;
	int ret;
	bool read_cmd = op->cmd & EIO_FLAG_PMC_READ;
	ktime_t t = ktime_get();

	mutex_lock(&eio->mutex);

	pmc_clear(dev, op->chip);

	ret = pmc_write_cmd(dev, op->chip, op->cmd, op->timeout);
	if (ret)
		goto err;

	ret = pmc_write_data(dev, op->chip, op->control, op->timeout);
	if (ret)
		goto err;

	ret = pmc_write_data(dev, op->chip, op->device_id, op->timeout);
	if (ret)
		goto err;

	ret = pmc_write_data(dev, op->chip, op->size, op->timeout);
	if (ret)
		goto err;

	for (i = 0; i < op->size; i++) {
		if (read_cmd)
			ret = pmc_read_data(dev, op->chip, &op->payload[i],
					    op->timeout);
		else
			ret = pmc_write_data(dev, op->chip, op->payload[i],
					     op->timeout);

		if (ret)
			goto err;
	}

	mutex_unlock(&eio->mutex);

	return 0;

err:
	mutex_unlock(&eio->mutex);

	dev_err(dev, "PMC error duration:%lldus",
		ktime_to_us(ktime_sub(ktime_get(), t)));
	dev_err(dev,
		".cmd=0x%02X, .ctrl=0x%02X .id=0x%02X, .size=0x%02X .data=0x%02X%02X",
		op->cmd, op->control, op->device_id, op->size, op->payload[0],
		op->payload[1]);

	return ret;
}
EXPORT_SYMBOL_GPL(eio_core_pmc_operation);

static int get_pmc_port(struct device *dev, int id,
			struct eio_dev_port *port)
{
	struct eio_dev *eio = dev_get_drvdata(dev);
	struct _pmc_port *pmc = &eio->pmc[id];

	eio_pnp_enter(dev, port);

	/* Switch to PMC device page */
	eio_pnp_write(dev, port, EIO_LDN, EIO_LDN_PMC1);

	/* Active this device */
	eio_pnp_write(dev, port, EIO_LDAR, EIO_LDAR_LDACT);

	/* Get PMC cmd and data port */
	pmc->data = eio_pnp_read(dev, port, EIO_IOBA0H) << 8;
	pmc->data |= eio_pnp_read(dev, port, EIO_IOBA0L);
	pmc->cmd = eio_pnp_read(dev, port, EIO_IOBA1H) << 8;
	pmc->cmd |= eio_pnp_read(dev, port, EIO_IOBA1L);

	/* Disable IRQ */
	eio_pnp_write(dev, port, EIO_IRQCTRL, 0);

	eio_pnp_leave(dev, port);

	/* Make sure IO ports are not occupied */
	if (!devm_request_region(dev, pmc->data, 2, KBUILD_MODNAME)) {
		dev_err(dev, "Request region %X error\n", pmc->data);
		return -EBUSY;
	}

	return 0;
}

static int eio_init(struct device *dev)
{
	struct eio_dev *eio = dev_get_drvdata(dev);
	u16 chip_id = 0;
	u8 tmp = 0;
	int chip = 0;
	int ret = -ENOMEM;

	for (chip = 0; chip < ARRAY_SIZE(pnp_port); chip++) {
		struct eio_dev_port *port = pnp_port + chip;

		if (!devm_request_region(dev, pnp_port[chip].idx_port,
					 pnp_port[chip].data_port -
						 pnp_port[chip].idx_port,
					 KBUILD_MODNAME))
			continue;

		eio_pnp_enter(dev, port);

		chip_id = eio_pnp_read(dev, port, EIO_CHIPID1) << 8;
		chip_id |= eio_pnp_read(dev, port, EIO_CHIPID2);

		if (chip_id != EIO200_CHIPID && chip_id != EIO201_211_CHIPID)
			continue;

		/* Turn on the enable flag */
		tmp = eio_pnp_read(dev, port, EIO_SIOCTRL);
		tmp |= EIO_SIOCTRL_SIOEN;

		eio_pnp_write(dev, port, EIO_SIOCTRL, tmp);

		eio_pnp_leave(dev, port);

		ret = get_pmc_port(dev, chip, port);
		if (ret)
			return ret;

		if (chip == 0)
			eio->flag |= EIO_F_CHIP_EXIST;
		else
			eio->flag |= EIO_F_SUB_CHIP_EXIST;
	}

	return ret;
}

static uint8_t acpiram_access(struct device *dev, uint8_t offset)
{
	u8 val;
	int ret;
	int timeout = 0;
	struct eio_dev *eio = dev_get_drvdata(dev);

	/* We only store information on primary EC */
	int chip = 0;

	mutex_lock(&eio->mutex);

	pmc_clear(dev, chip);

	ret = pmc_write_cmd(dev, chip, EIO_PMC_CMD_ACPIRAM_READ, timeout);
	if (ret)
		goto err;

	ret = pmc_write_data(dev, chip, offset, timeout);
	if (ret)
		goto err;

	ret = pmc_write_data(dev, chip, sizeof(val), timeout);
	if (ret)
		goto err;

	ret = pmc_read_data(dev, chip, &val, timeout);
	if (ret)
		goto err;

err:
	mutex_unlock(&eio->mutex);
	return ret ? 0 : val;
}

static int firmware_code_base(struct device *dev)
{
	struct eio_dev *eio = dev_get_drvdata(dev);
	u8 ic_vendor, ic_code, code_base;

	ic_vendor = acpiram_access(dev, EIO_ACPIRAM_ICVENDOR);
	ic_code = acpiram_access(dev, EIO_ACPIRAM_ICCODE);
	code_base = acpiram_access(dev, EIO_ACPIRAM_CODEBASE);

	if (ic_vendor != 'R')
		return -ENODEV;

	if (ic_code != EIO200_ICCODE && ic_code != EIO201_ICCODE &&
	    ic_code != EIO211_ICCODE)
		goto err;

	if (code_base == EIO_ACPIRAM_CODEBASE_NEW) {
		eio->flag |= EIO_F_NEW_CODE_BASE;
		return 0;
	}

	if (code_base == 0 &&
	    (ic_code != EIO201_ICCODE && ic_code != EIO211_ICCODE)) {
		dev_info(dev, "Old code base not supported, yet.");
		return -ENODEV;
	}

err:
	/* Codebase error. This should only happen on firmware error. */
	dev_err(dev,
		"Codebase check fail: vendor: 0x%X, code: 0x%X, base: 0x%X\n",
		ic_vendor, ic_code, code_base);
	return -ENODEV;
}

static int eio_probe(struct device *dev, unsigned int id)
{
	int ret = 0;
	struct eio_dev *eio;

	eio = devm_kzalloc(dev, sizeof(*eio), GFP_KERNEL);
	if (!eio)
		return -ENOMEM;

	eio->dev = dev;
	mutex_init(&eio->mutex);

	eio->iomem = devm_ioport_map(dev, 0, EIO_SUB_PNP_DATA + 1);
	if (IS_ERR(eio->iomem))
		return PTR_ERR(eio->iomem);

	eio->map = devm_regmap_init_mmio(dev, eio->iomem, &pnp_regmap_config);
	if (IS_ERR(eio->map))
		return PTR_ERR(eio->map);

	/* publish instance for subdrivers (dev_get_drvdata(dev->parent)) */
	dev_set_drvdata(dev, eio);

	if (eio_init(dev)) {
		dev_dbg(dev, "No device found\n");
		return -ENODEV;
	}

	ret = firmware_code_base(dev);
	if (ret) {
		dev_err(dev, "Chip code base check fail\n");
		return ret; /* keep helper's return (e.g., -EIO) */
	}

	ret = devm_mfd_add_devices(dev, PLATFORM_DEVID_NONE,
				   mfd_devs, ARRAY_SIZE(mfd_devs),
				   NULL, 0, NULL);
	if (ret)
		dev_err(dev, "Cannot register child devices (error = %d)\n", ret);

	dev_dbg(dev, "Module insert completed\n");
	return 0;
}

static struct isa_driver eio_driver = {
	.probe    = eio_probe,

	.driver = {
		.name = "eio_core",
		.dev_groups = pmc_groups,
	},
};
module_isa_driver(eio_driver, 1);

MODULE_AUTHOR("Wenkai Chung <wenkai.chung@advantech.com.tw>");
MODULE_AUTHOR("Ramiro Oliveira <ramiro.oliveira@advantech.com>");
MODULE_DESCRIPTION("Advantech EIO series EC core driver");
MODULE_LICENSE("GPL");
