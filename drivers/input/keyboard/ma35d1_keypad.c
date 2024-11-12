// SPDX-License-Identifier: GPL-2.0+
/*
 *  MA35D1 keypad driver
 *  Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/input/matrix_keypad.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/bitops.h>
#include <linux/pm_wakeirq.h>

/* Keypad Interface Registers */
#define KPI_CONF		0x00
#define KPI_3KCONF		0x04
#define KPI_STATUS		0x08
#define KPI_RSTC		0x0C
#define KPI_KEST		0x10
#define KPI_KPE0		0x18
#define KPI_KPE1		0x1C
#define KPI_KRE0		0x20
#define KPI_KRE1		0x24
#define KPI_PRESCALDIV		0x28

/* KPI_CONF - Keypad Configuration Register */
#define KROW		GENMASK(30, 28) /* Keypad Matrix ROW number */
#define KCOL		GENMASK(26, 24) /* Keypad Matrix COL Number */
#define DB_CLKSEL	GENMASK(19, 16) /* De-bounce sampling cycle selection */
#define PRESCALE	GENMASK(15, 8)  /* Row Scan Cycle Pre-scale Value */
#define WAKEUP		BIT(5) /* Lower Power Wakeup Enable */
#define INTEN		BIT(3) /* Key Interrupt Enable Control */
#define RKINTEN	BIT(2) /* Release Key Interrupt Enable */
#define PKINTEN	BIT(1) /* Press Key Interrupt Enable Control */
#define ENKP		BIT(0) /* Keypad Scan Enable */

/* KPI_STATUS - Keypad Status Register */
#define PKEY_INT	BIT(4) /* Press key interrupt */
#define RKEY_INT	BIT(3) /* Release key interrupt */
#define KEY_INT	BIT(2) /* Key Interrupt */
#define RST_3KEY	BIT(1) /* 3-Keys Reset Flag */
#define PDWAKE		BIT(0) /* Power Down Wakeup Flag */

#define KEY_EVENT_BITS 64

struct ma35d1_keypad {
	struct clk *clk;
	struct input_dev *input_dev;
	void __iomem *mmio_base;
	int irq;
	unsigned int kpi_row;
	unsigned int kpi_col;
	unsigned int debounce_val;
	unsigned int pre_scale;
	unsigned int pre_scale_divider;
};

static void ma35d1_keypad_scan_matrix(struct ma35d1_keypad *keypad, unsigned int status)
{
	struct input_dev *input_dev = keypad->input_dev;
	unsigned int code;
	unsigned int key;
	unsigned long pressed_keys = 0, released_keys = 0;
	unsigned int row_shift = get_count_order(keypad->kpi_col);
	unsigned short *keymap = input_dev->keycode;
	unsigned long key_event[4];
	unsigned int index;

	/* Read key event status */
	key_event[0] = readl(keypad->mmio_base + KPI_KPE0);
	key_event[1] = readl(keypad->mmio_base + KPI_KPE1);
	key_event[2] = readl(keypad->mmio_base + KPI_KRE0);
	key_event[3] = readl(keypad->mmio_base + KPI_KRE1);

	/* Clear key event status */
	writel(key_event[0], (keypad->mmio_base + KPI_KPE0));
	writel(key_event[1], (keypad->mmio_base + KPI_KPE1));
	writel(key_event[2], (keypad->mmio_base + KPI_KRE0));
	writel(key_event[3], (keypad->mmio_base + KPI_KRE1));

	pressed_keys  = key_event[0] | key_event[1] << 32;
	released_keys = key_event[2] | key_event[3] << 32;

	/* Process pressed keys */
	for_each_set_bit(index, &pressed_keys, KEY_EVENT_BITS) {
		code = MATRIX_SCAN_CODE(index / 8, (index % 8), row_shift);
		key = keymap[code];

		input_event(input_dev, EV_MSC, MSC_SCAN, code);
		input_report_key(input_dev, key, 1);
	}

	/* Process released keys */
	for_each_set_bit(index, &released_keys, KEY_EVENT_BITS) {
		code = MATRIX_SCAN_CODE(index / 8, (index % 8), row_shift);
		key = keymap[code];

		input_event(input_dev, EV_MSC, MSC_SCAN, code);
		input_report_key(input_dev, key, 0);
	}

	input_sync(input_dev);
}

static irqreturn_t ma35d1_keypad_interrupt(int irq, void *dev_id)
{
	struct ma35d1_keypad *keypad = dev_id;
	unsigned int  kstatus;

	kstatus = readl(keypad->mmio_base + KPI_STATUS);

	if (kstatus & (PKEY_INT | RKEY_INT)) {
		ma35d1_keypad_scan_matrix(keypad, kstatus);
	} else {
		if (kstatus & PDWAKE)
			writel(PDWAKE, (keypad->mmio_base + KPI_STATUS));
	}

	return IRQ_HANDLED;
}

static int ma35d1_keypad_open(struct input_dev *dev)
{
	struct ma35d1_keypad *keypad = input_get_drvdata(dev);
	unsigned int val, config;

	val = RKINTEN | PKINTEN | INTEN | ENKP;
	val |= FIELD_PREP(KCOL, (keypad->kpi_col - 1)) | FIELD_PREP(KROW, (keypad->kpi_row - 1));

	if (keypad->debounce_val > 0)
		config = FIELD_PREP(PRESCALE, (keypad->pre_scale - 1)) |
			 FIELD_PREP(DB_CLKSEL, keypad->debounce_val);
	else
		config = FIELD_PREP(PRESCALE, (keypad->pre_scale - 1));

	val |= config;

	writel(val, keypad->mmio_base + KPI_CONF);
	writel((keypad->pre_scale_divider - 1),	keypad->mmio_base + KPI_PRESCALDIV);

	return 0;
}

static void ma35d1_keypad_close(struct input_dev *dev)
{
	struct ma35d1_keypad *keypad = input_get_drvdata(dev);
	unsigned int val;

	val = readl(keypad->mmio_base + KPI_KPE0) & ~ENKP;
	writel(val, keypad->mmio_base + KPI_CONF);
}

static int ma35d1_keypad_probe(struct platform_device *pdev)
{
	struct ma35d1_keypad *keypad;
	struct input_dev *input_dev;
	struct resource *res;
	int error = 0;

	keypad = devm_kzalloc(&pdev->dev, sizeof(*keypad), GFP_KERNEL);
	if (!keypad)
		return -ENOMEM;

	input_dev = devm_input_allocate_device(&pdev->dev);
	if (!input_dev)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	keypad->mmio_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(keypad->mmio_base))
		return dev_err_probe(&pdev->dev, PTR_ERR(keypad->mmio_base),
					"failed to remap I/O memor\n");

	keypad->irq = platform_get_irq(pdev, 0);
	if (keypad->irq < 0) {
		dev_err(&pdev->dev, "failed to get IRQ\n");
		return keypad->irq;
	}

	keypad->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(keypad->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(keypad->clk), "failed to get core clk\n");

	error = matrix_keypad_parse_properties(&pdev->dev, &keypad->kpi_row, &keypad->kpi_col);
	if (error) {
		dev_err(&pdev->dev, "failed to parse keypad params\n");
		return error;
	}

	error = matrix_keypad_build_keymap(NULL, NULL, keypad->kpi_row, keypad->kpi_col,
					   NULL, input_dev);
	if (error) {
		dev_err(&pdev->dev, "failed to build keymap\n");
		return error;
	}

	keypad->input_dev = input_dev;
	input_dev->name = pdev->name;
	input_dev->id.bustype = BUS_HOST;
	input_dev->open = ma35d1_keypad_open;
	input_dev->close = ma35d1_keypad_close;
	input_dev->dev.parent = &pdev->dev;

	error = device_property_read_u32(&pdev->dev, "debounce-period", &keypad->debounce_val);
	if (error) {
		dev_err(&pdev->dev, "failed to acquire 'debounce-period'\n");
		return error;
	}

	keypad->debounce_val = __builtin_ctz(keypad->debounce_val);

	error = device_property_read_u32(&pdev->dev, "key-scan-time", &keypad->pre_scale);
	if (error) {
		dev_err(&pdev->dev, "failed to acquire 'key-scan-time'\n");
		return error;
	}

	device_property_read_u32(&pdev->dev, "key-scan-time-div", &keypad->pre_scale_divider);
	if (error) {
		dev_err(&pdev->dev, "failed to acquire 'key-scan-time-div'\n");
		return error;
	}

	__set_bit(EV_REP, input_dev->evbit);
	input_set_drvdata(input_dev, keypad);
	input_set_capability(input_dev, EV_MSC, MSC_SCAN);

	error = devm_request_irq(&pdev->dev, keypad->irq, ma35d1_keypad_interrupt,
				 IRQF_NO_SUSPEND, pdev->name, keypad);
	if (error) {
		dev_err(&pdev->dev, "failed to request IRQ\n");
		return error;
	}

	platform_set_drvdata(pdev, keypad);
	device_init_wakeup(&pdev->dev, 1);

	error = dev_pm_set_wake_irq(&pdev->dev, keypad->irq);
	if (error) {
		dev_err(&pdev->dev, "failed to enable irq wake\n");
		return error;
	}

	error = input_register_device(input_dev);
	if (error) {
		dev_err(&pdev->dev, "failed to register input device\n");
		return error;
	}

	return 0;
}

static void ma35d1_keypad_remove(struct platform_device *pdev)
{
	struct ma35d1_keypad *keypad = platform_get_drvdata(pdev);

	input_unregister_device(keypad->input_dev);
}

static int ma35d1_keypad_suspend(struct device *dev)
{
	struct ma35d1_keypad *keypad = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		writel(readl(keypad->mmio_base + KPI_CONF) | WAKEUP, keypad->mmio_base + KPI_CONF);

	return 0;
}

static int ma35d1_keypad_resume(struct device *dev)
{
	struct ma35d1_keypad *keypad = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		writel(readl(keypad->mmio_base + KPI_CONF) & ~(WAKEUP),
		       keypad->mmio_base + KPI_CONF);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ma35d1_pm_ops, ma35d1_keypad_suspend, ma35d1_keypad_resume);

static const struct of_device_id ma35d1_kpi_of_match[] = {
	{ .compatible = "nuvoton,ma35d1-kpi"},
	{},
};
MODULE_DEVICE_TABLE(of, ma35d1_kpi_of_match);

static struct platform_driver ma35d1_keypad_driver = {
	.probe		= ma35d1_keypad_probe,
	.remove		= ma35d1_keypad_remove,
	.driver		= {
		.name	= "ma35d1-kpi",
		.pm	= pm_sleep_ptr(&ma35d1_pm_ops),
		.of_match_table = ma35d1_kpi_of_match,
	},
};
module_platform_driver(ma35d1_keypad_driver);

MODULE_AUTHOR("Ming-Jen Chen");
MODULE_DESCRIPTION("MA35D1 Keypad Driver");
MODULE_LICENSE("GPL");

