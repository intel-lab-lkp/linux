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
#define KPI_PRESCALDIV	0x28

/* KPI_CONF - Keypad Configuration Register */
#define KROW		GENMASK(30, 28) /* Keypad Matrix ROW number */
#define KCOL		GENMASK(26, 24) /* Keypad Matrix COL Number */
#define DB_CLKSEL	GENMASK(19, 16) /* De-bounce sampling cycle selection */
#define PRESCALE	GENMASK(15, 8)  /* Row Scan Cycle Pre-scale Value */
#define WAKEUP		BIT(5) /* Lower Power Wakeup Enable */
#define INTEN		BIT(3) /* Key Interrupt Enable Control */
#define RKINTEN		BIT(2) /* Release Key Interrupt Enable */
#define PKINTEN		BIT(1) /* Press Key Interrupt Enable Control */
#define ENKP		BIT(0) /* Keypad Scan Enable */

/* KPI_STATUS - Keypad Status Register */
#define PKEY_INT	BIT(4) /* Press key interrupt */
#define RKEY_INT	BIT(3) /* Release key interrupt */
#define KEY_INT		BIT(2) /* Key Interrupt */
#define RST_3KEY	BIT(1) /* 3-Keys Reset Flag */
#define PDWAKE		BIT(0) /* Power Down Wakeup Flag */

#define DEFAULT_DEBOUNCE		1
#define DEFAULT_PRE_SCALE		1
#define DEFAULT_PRE_SCALEDIV	32

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

static void ma35d1_keypad_scan_matrix(struct ma35d1_keypad *keypad,	unsigned int status)
{
	struct input_dev *input_dev = keypad->input_dev;
	unsigned int i, j;
	unsigned int row_add = 0;
	unsigned int code;
	unsigned int key;
	unsigned int press_key;
	unsigned long KeyEvent[4];
	unsigned int row_shift = get_count_order(keypad->kpi_col);
	unsigned short *keymap = input_dev->keycode;

	/* Read key event status */
	KeyEvent[0] = readl(keypad->mmio_base + KPI_KPE0);
	KeyEvent[1] = readl(keypad->mmio_base + KPI_KPE1);
	KeyEvent[2] = readl(keypad->mmio_base + KPI_KRE0);
	KeyEvent[3] = readl(keypad->mmio_base + KPI_KRE1);

	/* Clear key event status */
	writel(KeyEvent[0], (keypad->mmio_base + KPI_KPE0));
	writel(KeyEvent[1], (keypad->mmio_base + KPI_KPE1));
	writel(KeyEvent[2], (keypad->mmio_base + KPI_KRE0));
	writel(KeyEvent[3], (keypad->mmio_base + KPI_KRE1));

	for (j = 0; j < 4; j++) {
		if (KeyEvent[j] != 0) {
			row_add = (j % 2) ? 4 : 0;
			press_key = (j < 2) ? 1 : 0;

			for (i = 0; i < 32; i++) {
				if (KeyEvent[j] & (1<<i)) {
					code = MATRIX_SCAN_CODE(((i/8) + row_add), (i % 8), row_shift);
					key = keymap[code];

					input_event(input_dev, EV_MSC, MSC_SCAN, code);
					input_report_key(input_dev, key, press_key);
				}
			}
		}
	}

	input_sync(input_dev);
}

static irqreturn_t ma35d1_keypad_interrupt(int irq, void *dev_id)
{
	struct ma35d1_keypad *keypad = dev_id;
	unsigned int  kstatus;

	kstatus = readl(keypad->mmio_base + KPI_STATUS);

	if (kstatus & (PKEY_INT|RKEY_INT)) {
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

	clk_disable(keypad->clk);
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


	input_dev = input_allocate_device();
	if (!input_dev) {
		dev_err(&pdev->dev, "failed to allocate input device\n");
		return -ENOMEM;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "failed to get I/O memory\n");
		error = -ENXIO;
		goto failed_free_input;
	}

	keypad->mmio_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(keypad->mmio_base)) {
		dev_err(&pdev->dev, "failed to remap I/O memory\n");
		return PTR_ERR(keypad->mmio_base);
	}

	keypad->irq = platform_get_irq(pdev, 0);
	if (keypad->irq < 0) {
		dev_err(&pdev->dev, "failed to get IRQ\n");
		return keypad->irq;
	}

	keypad->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(keypad->clk)) {
		dev_err(&pdev->dev, "failed to get core clk: %ld\n", PTR_ERR(keypad->clk));
		return PTR_ERR(keypad->clk);
	}

	error = matrix_keypad_parse_properties(&pdev->dev,
										   &(keypad->kpi_row),
										   &(keypad->kpi_col));
	if (error) {
		dev_err(&pdev->dev, "failed to parse kp params\n");
		return error;
	}

	error = matrix_keypad_build_keymap(NULL, NULL,
									   keypad->kpi_row,
									   keypad->kpi_col,
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

	if (of_property_read_u32(pdev->dev.of_node, "debounce-period", &(keypad->debounce_val)))
		keypad->debounce_val = DEFAULT_DEBOUNCE;

	if (of_property_read_u32(pdev->dev.of_node, "per-scale", &(keypad->pre_scale)))
		keypad->pre_scale = DEFAULT_PRE_SCALE;

	if (of_property_read_u32(pdev->dev.of_node, "per-scalediv", &(keypad->pre_scale_divider)))
		keypad->pre_scale_divider = DEFAULT_PRE_SCALEDIV;

	__set_bit(EV_REP, input_dev->evbit);
	input_set_drvdata(input_dev, keypad);
	input_set_capability(input_dev, EV_MSC, MSC_SCAN);

	error = input_register_device(input_dev);
	if (error) {
		dev_err(&pdev->dev, "failed to register input device\n");
		goto failed_free_input;
	}

	error = devm_request_irq(&pdev->dev, keypad->irq,
							 ma35d1_keypad_interrupt,
							 IRQF_NO_SUSPEND, pdev->name, keypad);
	if (error) {
		dev_err(&pdev->dev, "failed to request IRQ\n");
		goto failed_unregister_input;
	}

	platform_set_drvdata(pdev, keypad);
	device_init_wakeup(&pdev->dev, 1);
	clk_prepare_enable(keypad->clk);

	return 0;

failed_unregister_input:
	input_unregister_device(input_dev);
failed_free_input:
	input_free_device(input_dev);
	return error;
}

static void ma35d1_keypad_remove(struct platform_device *pdev)
{
	struct ma35d1_keypad *keypad = platform_get_drvdata(pdev);

	input_unregister_device(keypad->input_dev);
	clk_disable_unprepare(keypad->clk);
}

static int ma35d1_keypad_suspend(struct platform_device *pdev,
									pm_message_t state)
{
	struct ma35d1_keypad *keypad = platform_get_drvdata(pdev);

	if (device_may_wakeup(&pdev->dev)) {
		writel(readl(keypad->mmio_base + KPI_CONF) | WAKEUP, keypad->mmio_base + KPI_CONF);
		enable_irq_wake(keypad->irq);
	}

	return 0;
}

static int ma35d1_keypad_resume(struct platform_device *pdev)
{
	struct ma35d1_keypad *keypad = platform_get_drvdata(pdev);

	if (device_may_wakeup(&pdev->dev)) {
		writel(readl(keypad->mmio_base + KPI_CONF) & ~(WAKEUP),
						keypad->mmio_base + KPI_CONF);
		disable_irq_wake(keypad->irq);
	}

	return 0;
}

static const struct of_device_id ma35d1_kpi_of_match[] = {
	{ .compatible = "nuvoton,ma35d1-kpi"},
	{},
};
MODULE_DEVICE_TABLE(of, ma35d1_kpi_of_match);

static struct platform_driver ma35d1_keypad_driver = {
	.probe		= ma35d1_keypad_probe,
	.remove		= ma35d1_keypad_remove,
	.suspend	= ma35d1_keypad_suspend,
	.resume		= ma35d1_keypad_resume,
	.driver		= {
		.name	= "ma35d1-kpi",
		.of_match_table = of_match_ptr(ma35d1_kpi_of_match),
	},
};
module_platform_driver(ma35d1_keypad_driver);

MODULE_AUTHOR("Ming-Jen Chen");
MODULE_DESCRIPTION("MA35D1 Keypad Driver");
MODULE_LICENSE("GPL");

