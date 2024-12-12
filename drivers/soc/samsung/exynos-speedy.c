// SPDX-License-Identifier: GPL-2.0
/*
 * exynos-speedy.c - Samsung Exynos SPEEDY Host Controller Driver
 *
 * Copyright 2024, Markuss Broks <markuss.broks@gmail.com>
 * Copyright 2024, Maksym Holovach <nergzd@nergzd723.xyz>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include <linux/soc/samsung/exynos-speedy.h>

/* Speedy MMIO register map */
#define SPEEDY_CTRL					0x000
#define SPEEDY_FIFO_CTRL				0x004
#define SPEEDY_CMD					0x008
#define SPEEDY_INT_ENABLE				0x00C
#define SPEEDY_INT_STATUS				0x010
#define SPEEDY_FIFO_STATUS				0x030
#define SPEEDY_TX_DATA					0x034
#define SPEEDY_RX_DATA					0x038
#define SPEEDY_PACKET_GAP_TIME				0x044
#define SPEEDY_TIMEOUT_COUNT				0x048
#define SPEEDY_FIFO_DEBUG				0x100
#define SPEEDY_CTRL_STATUS				0x104

/* SPEEDY_CTRL register bits */
#define SPEEDY_ENABLE					(1 << 0)
#define SPEEDY_TIMEOUT_CMD_DISABLE			(1 << 1)
#define SPEEDY_TIMEOUT_STANDBY_DISABLE			(1 << 2)
#define SPEEDY_TIMEOUT_DATA_DISABLE			(1 << 3)
#define SPEEDY_ALWAYS_PULLUP_EN				(1 << 7)
#define SPEEDY_DATA_WIDTH_8BIT				(0 << 8)
#define SPEEDY_REMOTE_RESET_REQ				(1 << 30)
#define SPEEDY_SW_RST					(1 << 31)

/* SPEEDY_FIFO_CTRL register bits */
#define SPEEDY_RX_TRIGGER_LEVEL(x)			((x) << 0)
#define SPEEDY_TX_TRIGGER_LEVEL(x)			((x) << 8)
#define SPEEDY_FIFO_RESET				(1 << 31)

/* SPEEDY_CMD register bits */
#define SPEEDY_BURST_LENGTH(x)				((x) << 0)
#define SPEEDY_BURST_FIXED				(0 << 5)
#define SPEEDY_BURST_INCR				(1 << 5)
#define SPEEDY_BURST_EXTENSION				(2 << 5)
#define SPEEDY_ACCESS_BURST				(0 << 19)
#define SPEEDY_ACCESS_RANDOM				(1 << 19)
#define SPEEDY_DIRECTION_READ				(0 << 20)
#define SPEEDY_DIRECTION_WRITE				(1 << 20)

/* SPEEDY_INT_ENABLE register bits */
#define SPEEDY_TRANSFER_DONE_EN				(1 << 0)
#define SPEEDY_TIMEOUT_CMD_EN				(1 << 1)
#define SPEEDY_TIMEOUT_STANDBY_EN			(1 << 2)
#define SPEEDY_TIMEOUT_DATA_EN				(1 << 3)
#define SPEEDY_FIFO_TX_ALMOST_EMPTY_EN			(1 << 4)
#define SPEEDY_FIFO_RX_ALMOST_FULL_EN			(1 << 8)
#define SPEEDY_RX_FIFO_INT_TRAILER_EN			(1 << 9)
#define SPEEDY_RX_MODEBIT_ERR_EN			(1 << 16)
#define SPEEDY_RX_GLITCH_ERR_EN				(1 << 17)
#define SPEEDY_RX_ENDBIT_ERR_EN				(1 << 18)
#define SPEEDY_TX_LINE_BUSY_ERR_EN			(1 << 20)
#define SPEEDY_TX_STOPBIT_ERR_EN			(1 << 21)
#define SPEEDY_REMOTE_RESET_REQ_EN			(1 << 31)

/* SPEEDY_INT_STATUS register bits */
#define SPEEDY_TRANSFER_DONE				(1 << 0)
#define SPEEDY_TIMEOUT_CMD				(1 << 1)
#define SPEEDY_TIMEOUT_STANDBY				(1 << 2)
#define SPEEDY_TIMEOUT_DATA				(1 << 3)
#define SPEEDY_FIFO_TX_ALMOST_EMPTY			(1 << 4)
#define SPEEDY_FIFO_RX_ALMOST_FULL			(1 << 8)
#define SPEEDY_RX_FIFO_INT_TRAILER			(1 << 9)
#define SPEEDY_RX_MODEBIT_ERR				(1 << 16)
#define SPEEDY_RX_GLITCH_ERR				(1 << 17)
#define SPEEDY_RX_ENDBIT_ERR				(1 << 18)
#define SPEEDY_TX_LINE_BUSY_ERR				(1 << 20)
#define SPEEDY_TX_STOPBIT_ERR				(1 << 21)
#define SPEEDY_REMOTE_RESET_REQ_STAT			(1 << 31)

/* SPEEDY_FIFO_STATUS register bits */
#define SPEEDY_VALID_DATA_CNT				(0 << 0)
#define SPEEDY_FIFO_FULL				(1 << 5)
#define SPEEDY_FIFO_EMPTY				(1 << 6)

/* SPEEDY_PACKET_GAP_TIME register bits */
#define SPEEDY_FIFO_TX_ALMOST_EMPTY			(1 << 4)
#define SPEEDY_FIFO_RX_ALMOST_FULL			(1 << 8)
#define SPEEDY_FSM_INIT					(1 << 1)
#define SPEEDY_FSM_TX_CMD				(1 << 2)
#define SPEEDY_FSM_STANDBY				(1 << 3)
#define SPEEDY_FSM_DATA					(1 << 4)
#define SPEEDY_FSM_TIMEOUT				(1 << 5)
#define SPEEDY_FSM_TRANS_DONE				(1 << 6)
#define SPEEDY_FSM_IO_RX_STAT_MASK			(3 << 7)
#define SPEEDY_FSM_IO_TX_IDLE				(1 << 9)
#define SPEEDY_FSM_IO_TX_GET_PACKET			(1 << 10)
#define SPEEDY_FSM_IO_TX_PACKET				(1 << 11)
#define SPEEDY_FSM_IO_TX_DONE				(1 << 12)

#define SPEEDY_RX_LENGTH(n)				((n) << 0)
#define SPEEDY_TX_LENGTH(n)				((n) << 8)

#define SPEEDY_DEVICE(x)				((x & 0xf) << 15)
#define SPEEDY_ADDRESS(x)				((x & 0xff) << 7)

static const struct of_device_id speedy_match[] = {
	{ .compatible = "samsung,exynos9810-speedy" },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, speedy_match);

static const struct regmap_config speedy_map_cfg = {
	.reg_bits = 32,
	.val_bits = 32,
};

/**
 * speedy_int_clear() - clear interrupt status
 * @speedy:	pointer to speedy controller struct
 *
 * Return: 0 on success, -errno otherwise
 */
static int speedy_int_clear(struct speedy_controller *speedy)
{
	int ret;

	ret = regmap_set_bits(speedy->map, SPEEDY_INT_STATUS, 0xffffffff);
	if (ret)
		return ret;

	udelay(10);

	return 0;
}

/**
 * speedy_fifo_reset() - reset FIFO of the controller
 * @speedy:	pointer to speedy controller struct
 *
 * Return: 0 on success, -errno otherwise
 */
static int speedy_fifo_reset(struct speedy_controller *speedy)
{
	int ret;

	ret = regmap_set_bits(speedy->map, SPEEDY_FIFO_CTRL, SPEEDY_FIFO_RESET);
	if (ret)
		return ret;

	udelay(10);

	return 0;
}

/**
 * _speedy_read() - internal speedy read operation
 * @speedy:	pointer to speedy controller struct
 * @reg:	address of device on the bus
 * @addr:       address to read
 * @val:        pointer to store result
 *
 * Return: 0 on success, -errno otherwise
 */
static int _speedy_read(struct speedy_controller *speedy, u32 reg, u32 addr, u32 *val)
{
	int ret;
	u32 cmd, int_ctl, int_status;

	mutex_lock(&speedy->io_lock);

	ret = speedy_fifo_reset(speedy);
	if (ret)
		return ret;

	ret = regmap_set_bits(speedy->map, SPEEDY_FIFO_CTRL,
			      SPEEDY_RX_LENGTH(1) | SPEEDY_TX_LENGTH(1));
	if (ret)
		return ret;

	cmd = SPEEDY_ACCESS_RANDOM | SPEEDY_DIRECTION_READ |
	      SPEEDY_DEVICE(reg) | SPEEDY_ADDRESS(addr);

	int_ctl = SPEEDY_TRANSFER_DONE_EN | SPEEDY_FIFO_RX_ALMOST_FULL_EN |
		  SPEEDY_RX_FIFO_INT_TRAILER_EN | SPEEDY_RX_MODEBIT_ERR_EN |
		  SPEEDY_RX_GLITCH_ERR_EN | SPEEDY_RX_ENDBIT_ERR_EN |
		  SPEEDY_REMOTE_RESET_REQ_EN;

	ret = speedy_int_clear(speedy);
	if (ret)
		return ret;

	ret = regmap_write(speedy->map, SPEEDY_INT_ENABLE, int_ctl);
	if (ret)
		return ret;

	ret = regmap_write(speedy->map, SPEEDY_CMD, cmd);
	if (ret)
		return ret;

	/* Wait for xfer done */
	ret = regmap_read_poll_timeout(speedy->map, SPEEDY_INT_STATUS, int_status,
				       int_status & SPEEDY_TRANSFER_DONE, 5000, 50000);
	if (ret)
		return ret;

	ret = regmap_read(speedy->map, SPEEDY_RX_DATA, val);
	if (ret)
		return ret;

	ret = speedy_int_clear(speedy);

	mutex_unlock(&speedy->io_lock);

	return ret;
}

int exynos_speedy_read(const struct speedy_device *device, u32 addr, u32 *val)
{
	return _speedy_read(device->speedy, device->reg, addr, val);
}
EXPORT_SYMBOL_GPL(exynos_speedy_read);

/**
 * _speedy_write() - internal speedy write operation
 * @speedy:	pointer to speedy controller struct
 * @reg:	address of device on the bus
 * @addr:       address to write
 * @val:        value to write
 *
 * Return: 0 on success, -errno otherwise
 */
static int _speedy_write(struct speedy_controller *speedy, u32 reg, u32 addr, u32 val)
{
	int ret;
	u32 cmd, int_ctl, int_status;

	mutex_lock(&speedy->io_lock);

	ret = speedy_fifo_reset(speedy);
	if (ret)
		return ret;

	ret = regmap_set_bits(speedy->map, SPEEDY_FIFO_CTRL,
			      SPEEDY_RX_LENGTH(1) | SPEEDY_TX_LENGTH(1));
	if (ret)
		return ret;

	cmd = SPEEDY_ACCESS_RANDOM | SPEEDY_DIRECTION_WRITE |
	      SPEEDY_DEVICE(reg) | SPEEDY_ADDRESS(addr);

	int_ctl = (SPEEDY_TRANSFER_DONE_EN |
		   SPEEDY_FIFO_TX_ALMOST_EMPTY_EN |
		   SPEEDY_TX_LINE_BUSY_ERR_EN |
		   SPEEDY_TX_STOPBIT_ERR_EN |
		   SPEEDY_REMOTE_RESET_REQ_EN);

	ret = speedy_int_clear(speedy);
	if (ret)
		return ret;

	ret = regmap_write(speedy->map, SPEEDY_INT_ENABLE, int_ctl);
	if (ret)
		return ret;

	ret = regmap_write(speedy->map, SPEEDY_CMD, cmd);
	if (ret)
		return ret;

	ret = regmap_write(speedy->map, SPEEDY_TX_DATA, val);
	if (ret)
		return ret;

	/* Wait for xfer done */
	ret = regmap_read_poll_timeout(speedy->map, SPEEDY_INT_STATUS, int_status,
				       int_status & SPEEDY_TRANSFER_DONE, 5000, 50000);
	if (ret)
		return ret;

	speedy_int_clear(speedy);

	mutex_unlock(&speedy->io_lock);

	return 0;
}

int exynos_speedy_write(const struct speedy_device *device, u32 addr, u32 val)
{
	return _speedy_write(device->speedy, device->reg, addr, val);
}
EXPORT_SYMBOL_GPL(exynos_speedy_write);

static void devm_speedy_release(struct device *dev, void *res)
{
	const struct speedy_device **ptr = res;
	const struct speedy_device *handle = *ptr;

	kfree(handle);
}

/**
 * speedy_get_by_phandle() - internal get speedy device handle
 * @np:	pointer to OF device node of device
 *
 * Return: 0 on success, -errno otherwise
 */
static const struct speedy_device *speedy_get_device(struct device_node *np)
{
	const struct of_device_id *speedy_id;
	struct device_node *speedy_np;
	struct platform_device *speedy_pdev;
	struct speedy_controller *speedy = NULL;
	struct speedy_device *handle;
	int ret;

	if (!np) {
		pr_err("I need a device pointer\n");
		return ERR_PTR(-EINVAL);
	}

	speedy_np = of_get_parent(np);
	if (!speedy_np)
		return ERR_PTR(-ENODEV);

	/* Verify if parent node is a speedy controller */
	speedy_id = of_match_node(speedy_match, speedy_np);
	if (!speedy_id) {
		handle = ERR_PTR(-EINVAL);
		goto out;
	}

	/* Get platform device of the speedy controller */
	speedy_pdev = of_find_device_by_node(speedy_np);
	if (!speedy_pdev) {
		handle = ERR_PTR(-EPROBE_DEFER);
		goto out;
	}

	/* Get drvdata of speedy controller */
	speedy = platform_get_drvdata(speedy_pdev);
	if (!speedy) {
		handle = ERR_PTR(-EINVAL);
		goto out;
	}

	handle = kzalloc(sizeof(struct speedy_device), GFP_KERNEL);
	if (!handle) {
		handle = ERR_PTR(-ENOMEM);
		goto out;
	}
	handle->speedy = speedy;
	ret = of_property_read_u32(np, "reg", &handle->reg);
	if (ret) {
		kfree(handle);
		handle = ERR_PTR(-EINVAL);
		goto out;
	}

out:
	of_node_put(speedy_np);
	return handle;
}

const struct speedy_device *devm_speedy_get_device(struct device *dev)
{
	const struct speedy_device *handle;
	const struct speedy_device **ptr;

	ptr = devres_alloc(devm_speedy_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	handle = speedy_get_device(dev_of_node(dev));
	if (!IS_ERR(handle)) {
		*ptr = handle;
		devres_add(dev, ptr);
	} else {
		devres_free(ptr);
	}

	return handle;
}
EXPORT_SYMBOL_GPL(devm_speedy_get_device);

static int speedy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct speedy_controller *speedy;
	void __iomem *mem;
	int ret;

	speedy = devm_kzalloc(dev, sizeof(struct speedy_controller), GFP_KERNEL);
	if (!speedy)
		return -ENOMEM;

	platform_set_drvdata(pdev, speedy);
	speedy->pdev = pdev;

	mutex_init(&speedy->io_lock);

	mem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mem))
		return dev_err_probe(dev, PTR_ERR(mem), "Failed to ioremap memory\n");

	speedy->map = devm_regmap_init_mmio(dev, mem, &speedy_map_cfg);
	if (IS_ERR(speedy->map))
		return dev_err_probe(dev, PTR_ERR(speedy->map), "Failed to init the regmap\n");

	/* Clear any interrupt status remaining */
	ret = speedy_int_clear(speedy);
	if (ret)
		return ret;

	/* Reset the controller */
	ret = regmap_set_bits(speedy->map, SPEEDY_CTRL, SPEEDY_SW_RST);
	if (ret)
		return ret;

	msleep(20);

	/* Enable the hw */
	ret = regmap_set_bits(speedy->map, SPEEDY_CTRL, SPEEDY_ENABLE);
	if (ret)
		return ret;

	msleep(20);

	/* Probe child devices */
	ret = of_platform_populate(pdev->dev.of_node, NULL, NULL, dev);
	if (ret)
		dev_err(dev, "Failed to populate child devices: %d\n", ret);

	return ret;
}

static struct platform_driver speedy_driver = {
	.probe = speedy_probe,
	.driver = {
		.name = "exynos-speedy",
		.of_match_table = speedy_match,
	},
};

module_platform_driver(speedy_driver);

MODULE_DESCRIPTION("Samsung Exynos SPEEDY host controller driver");
MODULE_AUTHOR("Markuss Broks <markuss.broks@gmail.com>");
MODULE_LICENSE("GPL");
