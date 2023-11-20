// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPIO driver for the Advantech PCIe-1761H device
 * Copyright (C) 2023 Christian Seiler
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/gpio/driver.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/bitops.h>

#define ADVANTECH_VENDOR_ID 0x13fe
#define PCIE1761H_PRODUCT_ID 0x0071

#define PCIE1761H_META_BAR_INDEX 0
#define PCIE1761H_META_BAR_SIZE 128
#define PCIE1761H_CONTROL_BAR_INDEX 1
#define PCIE1761H_CONTROL_BAR_SIZE 1024

#define PCIE1761H_META_OFFSET_BOARD_ID 0x00
#define PCIE1761H_META_OFFSET_PCB_VER 0x04
#define PCIE1761H_META_OFFSET_FW_VER 0x08

#define PCIE1761H_OFFSET_IRQFLAG 0x000
#define PCIE1761H_OFFSET_DO 0x100
#define PCIE1761H_OFFSET_DI 0x104

#define PCIE1761H_OFFSET_DICTL_INT_FALLING_EDGE 0x162
#define PCIE1761H_OFFSET_DICTL_INT_RISING_EDGE 0x163

#define PCIE1761H_OFFSET_DICTL_DEBOUNCE_ENABLE 0x167
#define PCIE1761H_OFFSET_DICTL_DEBOUNCE_TIME 0x16C

static u8 adv_pcie_1761h_convert_debounce_timing(unsigned long time_us)
{
	/* Get the next power of 2, but only in a range from
	 * 2^4 (16) to 2^17 (131072). The actual setting value
	 * in the register then has an offset of 3. The mapping
	 * is thus 7 -> 16us, 8 -> 32us, ..., 20 -> 131072 us.
	 */
	if (time_us > 0)
		return (u8)clamp((int)fls_long(time_us - 1), 4, 17) + 3;
	else
		return 0;
}

static struct pci_device_id adv_pcie_1761h_ids[] = {
	{ PCI_DEVICE(ADVANTECH_VENDOR_ID, PCIE1761H_PRODUCT_ID) },
	{}
};
MODULE_DEVICE_TABLE(pci, adv_pcie_1761h_ids);

struct adv_pcie_1761h_gpiochip {
	void __iomem *metadata_regs;
	void __iomem *control_regs;
	struct gpio_chip chip;

	char pcb_version[16];
	char firmware_version[16];
	int board_id;
	char gpiochip_label[32];

	raw_spinlock_t lock;
	u8 irq_enabled;
	u8 irq_rising;
	u8 irq_falling;
	u8 irq_masked;
	u8 debounce[8];
};

static struct adv_pcie_1761h_gpiochip *
irq_data_to_adv_pcie_1761h_gpiochip(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);

	return container_of(gc, struct adv_pcie_1761h_gpiochip, chip);
}

static void adv_pcie_1761h_set_multiple(struct gpio_chip *chip,
					unsigned long *mask,
					unsigned long *bits)
{
	u8 val;
	struct adv_pcie_1761h_gpiochip *card = gpiochip_get_data(chip);

	val = ioread8(card->control_regs + PCIE1761H_OFFSET_DO);
	val = (val & *mask) | *bits;
	iowrite8(val, card->control_regs + PCIE1761H_OFFSET_DO);
}

static void adv_pcie_1761h_set(struct gpio_chip *chip, unsigned int channel,
			       int value)
{
	unsigned long mask = 0, bits = 0;

	if (channel >= 8)
		return;

	mask = ~(1 << channel);
	bits = (value << channel);
	adv_pcie_1761h_set_multiple(chip, &mask, &bits);
}

static int adv_pcie_1761h_get_multiple(struct gpio_chip *chip,
				       unsigned long *mask, unsigned long *bits)
{
	u8 val_out;
	u8 val_in;
	unsigned long raw_state;
	struct adv_pcie_1761h_gpiochip *card = gpiochip_get_data(chip);

	val_out = ioread8(card->control_regs + PCIE1761H_OFFSET_DO);
	val_in = ioread8(card->control_regs + PCIE1761H_OFFSET_DI);
	raw_state = val_out | (val_in << 8);
	*bits = (raw_state & *mask);
	return 0;
}

static int adv_pcie_1761h_get(struct gpio_chip *chip, unsigned int channel)
{
	unsigned long mask = 0, bits = 0;

	mask = (1 << channel);
	adv_pcie_1761h_get_multiple(chip, &mask, &bits);
	return bits ? 1 : 0;
}

static int adv_pcie_1761h_get_direction(struct gpio_chip *chip,
					unsigned int channel)
{
	if (channel < 8)
		return GPIO_LINE_DIRECTION_OUT;
	else
		return GPIO_LINE_DIRECTION_IN;
}

static int adv_pcie_1761h_direction_input(struct gpio_chip *chip,
					  unsigned int channel)
{
	if (channel < 8 || channel >= 16)
		return -EINVAL;
	return 0;
}

static int adv_pcie_1761h_direction_output(struct gpio_chip *chip,
					   unsigned int channel, int value)
{
	if (channel >= 8)
		return -EINVAL;

	adv_pcie_1761h_set(chip, channel, value);
	return 0;
}

/* This function assumes that the lock has already been taken by the
 * caller!
 */
static void adv_pcie_1761h_update_debounce(struct adv_pcie_1761h_gpiochip *card)
{
	/* The card has only a single debounce time setting for all
	 * 8 inputs, but can enable debounce on each input
	 * individually if necessary. We always use the max debounce
	 * time across all inputs for the debounce time register.
	 */
	u8 max_debounce = 0;

	for (int i = 0; i < 8; ++i)
		max_debounce = max(card->debounce[i], max_debounce);
	iowrite8(max_debounce,
		 card->control_regs + PCIE1761H_OFFSET_DICTL_DEBOUNCE_TIME);
}

static int adv_pcie_1761h_set_debounce(struct gpio_chip *chip,
				       unsigned int channel,
				       unsigned long time_us)
{
	struct adv_pcie_1761h_gpiochip *card = gpiochip_get_data(chip);
	u8 debounce_value = adv_pcie_1761h_convert_debounce_timing(time_us);
	u8 mask = BIT(channel - 8);
	u8 enable;
	unsigned long flags;

	if (channel < 8 || channel >= 16)
		return -EINVAL;

	raw_spin_lock_irqsave(&card->lock, flags);
	enable = ioread8(card->control_regs +
			 PCIE1761H_OFFSET_DICTL_DEBOUNCE_ENABLE);
	if (time_us > 0)
		enable |= mask;
	else
		enable &= ~mask;
	iowrite8(enable,
		 card->control_regs + PCIE1761H_OFFSET_DICTL_DEBOUNCE_ENABLE);
	card->debounce[channel - 8] = debounce_value;
	adv_pcie_1761h_update_debounce(card);
	raw_spin_unlock_irqrestore(&card->lock, flags);

	return 0;
}

static int adv_pcie_1761h_set_config(struct gpio_chip *chip,
				     unsigned int channel, unsigned long config)
{
	enum pin_config_param param = pinconf_to_config_param(config);

	switch (param) {
	case PIN_CONFIG_INPUT_DEBOUNCE:
		return adv_pcie_1761h_set_debounce(
			chip, channel, pinconf_to_config_argument(config));
	case PIN_CONFIG_PERSIST_STATE:
		/* FIXME: pretend that we support this via software, even
		 * though this can only be changed via a dip switch on the
		 * actual board, and we (probably) can't even read out the
		 * current setting.
		 */
		return 0;

	default:
		break;
	}

	return -EOPNOTSUPP;
}

static const char *const adv_pcie_1761h_line_names[] = {
	"RELAY0", "RELAY1", "RELAY2", "RELAY3", "RELAY4", "RELAY5",
	"RELAY6", "RELAY7", "IDI0",   "IDI1",	"IDI2",	  "IDI3",
	"IDI4",	  "IDI5",   "IDI6",   "IDI7",
};

static const struct gpio_chip template_chip = {
	.owner = THIS_MODULE,
	.get_direction = adv_pcie_1761h_get_direction,
	.direction_input = adv_pcie_1761h_direction_input,
	.direction_output = adv_pcie_1761h_direction_output,
	.set_config = adv_pcie_1761h_set_config,
	.set = adv_pcie_1761h_set,
	.set_multiple = adv_pcie_1761h_set_multiple,
	.get = adv_pcie_1761h_get,
	.get_multiple = adv_pcie_1761h_get_multiple,
	.base = -1,
	.ngpio = 16,
	.names = adv_pcie_1761h_line_names,
	.can_sleep = false,
};

/* This function assumes that the lock has already been taken by the
 * caller!
 */
static void adv_pcie_1761h_update_irq(struct adv_pcie_1761h_gpiochip *card)
{
	u8 new_rising = card->irq_enabled & card->irq_rising &
			~card->irq_masked;
	u8 new_falling = card->irq_enabled & card->irq_falling &
			 ~card->irq_masked;
	iowrite8(new_rising,
		 card->control_regs + PCIE1761H_OFFSET_DICTL_INT_RISING_EDGE);
	iowrite8(new_falling,
		 card->control_regs + PCIE1761H_OFFSET_DICTL_INT_FALLING_EDGE);
}

static void adv_pcie_1761h_irq_ack(struct irq_data *data)
{
}

static void adv_pcie_1761h_irq_unmask(struct irq_data *data)
{
	struct adv_pcie_1761h_gpiochip *card =
		irq_data_to_adv_pcie_1761h_gpiochip(data);
	unsigned long channel = irqd_to_hwirq(data);
	u8 mask = BIT(channel - 8);
	unsigned long flags;

	gpiochip_enable_irq(&card->chip, irqd_to_hwirq(data));
	raw_spin_lock_irqsave(&card->lock, flags);
	card->irq_masked |= mask;
	adv_pcie_1761h_update_irq(card);
	raw_spin_unlock_irqrestore(&card->lock, flags);
}

static void adv_pcie_1761h_irq_mask(struct irq_data *data)
{
	struct adv_pcie_1761h_gpiochip *card =
		irq_data_to_adv_pcie_1761h_gpiochip(data);
	unsigned long channel = irqd_to_hwirq(data);
	u8 mask = BIT(channel - 8);
	unsigned long flags;

	raw_spin_lock_irqsave(&card->lock, flags);
	card->irq_masked &= ~mask;
	adv_pcie_1761h_update_irq(card);
	raw_spin_unlock_irqrestore(&card->lock, flags);
	gpiochip_disable_irq(&card->chip, channel);
}

static void adv_pcie_1761h_irq_enable(struct irq_data *data)
{
	struct adv_pcie_1761h_gpiochip *card =
		irq_data_to_adv_pcie_1761h_gpiochip(data);
	unsigned long channel = irqd_to_hwirq(data);
	u8 mask = BIT(channel - 8);
	unsigned long flags;

	raw_spin_lock_irqsave(&card->lock, flags);
	card->irq_enabled |= mask;
	adv_pcie_1761h_update_irq(card);
	raw_spin_unlock_irqrestore(&card->lock, flags);
}

static void adv_pcie_1761h_irq_disable(struct irq_data *data)
{
	struct adv_pcie_1761h_gpiochip *card =
		irq_data_to_adv_pcie_1761h_gpiochip(data);
	unsigned long channel = irqd_to_hwirq(data);
	u8 mask = BIT(channel - 8);
	unsigned long flags;

	raw_spin_lock_irqsave(&card->lock, flags);
	card->irq_enabled &= ~mask;
	adv_pcie_1761h_update_irq(card);
	raw_spin_unlock_irqrestore(&card->lock, flags);
}

static int adv_pcie_1761h_irq_set_type(struct irq_data *data,
				       unsigned int flow_type)
{
	struct adv_pcie_1761h_gpiochip *card =
		irq_data_to_adv_pcie_1761h_gpiochip(data);
	unsigned long channel = irqd_to_hwirq(data);
	u8 mask = BIT(channel - 8);
	unsigned long flags;

	if (channel < 8 || channel >= 16)
		return -EINVAL;

	raw_spin_lock_irqsave(&card->lock, flags);
	switch (flow_type) {
	case IRQ_TYPE_EDGE_RISING:
		card->irq_rising |= mask;
		card->irq_falling &= ~mask;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		card->irq_rising &= ~mask;
		card->irq_falling |= mask;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		card->irq_rising |= mask;
		card->irq_falling |= mask;
		break;
	default:
		return -EINVAL;
	}
	adv_pcie_1761h_update_irq(card);
	raw_spin_unlock_irqrestore(&card->lock, flags);
	return 0;
}

static const struct irq_chip adv_pcie_1761h_gpio_irqchip = {
	.name = "gpio-adv-pcie-1761h",
	.irq_enable = adv_pcie_1761h_irq_enable,
	.irq_disable = adv_pcie_1761h_irq_disable,
	.irq_ack = adv_pcie_1761h_irq_ack,
	.irq_mask = adv_pcie_1761h_irq_mask,
	.irq_unmask = adv_pcie_1761h_irq_unmask,
	.irq_set_type = adv_pcie_1761h_irq_set_type,
	.flags = IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

irqreturn_t adv_pcie_1761h_irq_handler(int irq, void *data)
{
	struct adv_pcie_1761h_gpiochip *card = data;
	unsigned long pending;

	if (card == NULL)
		return IRQ_HANDLED;

	/* Read out which lines were set */
	raw_spin_lock(&card->lock);
	pending = ioread32(card->control_regs + PCIE1761H_OFFSET_IRQFLAG);
	raw_spin_unlock(&card->lock);

	if (!pending)
		return IRQ_NONE;

	for_each_set_bit(irq, &pending, card->chip.ngpio - 8)
		generic_handle_domain_irq(card->chip.irq.domain, irq + 8);

	/* Reset the IRQ flags for those lines */
	raw_spin_lock(&card->lock);
	iowrite32(pending, card->control_regs + PCIE1761H_OFFSET_IRQFLAG);
	raw_spin_unlock(&card->lock);

	return IRQ_HANDLED;
}

static int adv_pcie_1761h_probe(struct pci_dev *pdev,
				const struct pci_device_id *id)
{
	struct device *const dev = &pdev->dev;
	struct adv_pcie_1761h_gpiochip *card;
	int err;
	u32 pcb_ver, fw_ver;
	struct gpio_irq_chip *girq;

	err = pci_resource_len(pdev, PCIE1761H_META_BAR_INDEX);
	if (err != PCIE1761H_META_BAR_SIZE) {
		dev_err(dev,
			"Unsupported BAR%d size %d for Advantech PCIe-1761H (Expected %d)\n",
			PCIE1761H_META_BAR_INDEX, err, PCIE1761H_META_BAR_SIZE);
		return -1;
	}
	err = pci_resource_len(pdev, PCIE1761H_CONTROL_BAR_INDEX);
	if (err != PCIE1761H_CONTROL_BAR_SIZE) {
		dev_err(dev,
			"Unsupported BAR%d size %d for Advantech PCIe-1761H (Expected %d)\n",
			PCIE1761H_CONTROL_BAR_INDEX, err,
			PCIE1761H_CONTROL_BAR_SIZE);
		return -1;
	}

	err = pcim_enable_device(pdev);
	if (err < 0) {
		dev_err(dev, "could not enable the device\n");
		return err;
	}

	err = pcim_iomap_regions(pdev,
				 BIT(PCIE1761H_META_BAR_INDEX) |
					 BIT(PCIE1761H_CONTROL_BAR_INDEX),
				 KBUILD_MODNAME);
	if (err < 0) {
		dev_err(dev, "could not map BAR%d/BAR%d of device\n",
			PCIE1761H_META_BAR_INDEX, PCIE1761H_CONTROL_BAR_INDEX);
		return err;
	}

	card = devm_kzalloc(dev, sizeof(struct adv_pcie_1761h_gpiochip),
			    GFP_KERNEL);
	if (card == NULL)
		return -ENOMEM;

	card->metadata_regs = pcim_iomap_table(pdev)[PCIE1761H_META_BAR_INDEX];
	if (card->metadata_regs == NULL) {
		dev_err(dev, "invalid pointer for BAR%d\n",
			PCIE1761H_META_BAR_INDEX);
		return -1;
	}
	card->control_regs =
		pcim_iomap_table(pdev)[PCIE1761H_CONTROL_BAR_INDEX];
	if (card->control_regs == NULL) {
		dev_err(dev,
			"invalid pointer for BAR%d\n",
			PCIE1761H_CONTROL_BAR_INDEX);
		return -1;
	}

	/* The board id is a 4bit number that the user can set via a
	 * set of dip switches on the board; this can be used to
	 * distinguish multiple boards of the same type in the system.
	 * We'll encode the board id in the chip label so that
	 * userspace still has access to that information.
	 */
	card->board_id =
		ioread8(card->metadata_regs + PCIE1761H_META_OFFSET_BOARD_ID) &
		0x0f;
	snprintf(card->gpiochip_label, sizeof(card->gpiochip_label),
		 "adv-pcie-1761h:%d", card->board_id);

	/* Format the version numbers in the same manner as
	 * the Advantech software displays them on Windows.
	 */
	pcb_ver = ioread32(card->metadata_regs + PCIE1761H_META_OFFSET_PCB_VER);
	snprintf(card->pcb_version, sizeof(card->pcb_version), "%02X %02d-%d",
		 (pcb_ver >> 16) & 0xff, (pcb_ver >> 8) & 0xff, pcb_ver & 0xf);
	fw_ver = ioread32(card->metadata_regs + PCIE1761H_META_OFFSET_FW_VER);
	snprintf(card->firmware_version, sizeof(card->firmware_version),
		 "%d.%d.%d.%d", (fw_ver >> 24) & 0xff, (fw_ver >> 16) & 0xff,
		 (fw_ver >> 8) & 0xff, fw_ver & 0xff);

	dev_info(dev,
		 "Found Advantech PCIe-1761H with PCB version %s, firmware version %s, board id %d\n",
		 card->pcb_version, card->firmware_version, card->board_id);

	pci_set_drvdata(pdev, card);

	card->chip = template_chip;
	card->chip.label = card->gpiochip_label;
	card->chip.parent = dev;

	girq = &card->chip.irq;
	gpio_irq_chip_set_chip(girq, &adv_pcie_1761h_gpio_irqchip);
	/* This will let us handle the parent IRQ in the driver */
	girq->parent_handler = NULL;
	girq->num_parents = 0;
	girq->parents = NULL;
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_edge_irq;

	raw_spin_lock_init(&card->lock);

	/* Disable interrupts on the device in case they were still
	 * active previously.
	 */
	iowrite8(0x00,
		 card->control_regs + PCIE1761H_OFFSET_DICTL_INT_RISING_EDGE);
	iowrite8(0x00,
		 card->control_regs + PCIE1761H_OFFSET_DICTL_INT_FALLING_EDGE);
	card->irq_enabled = 0;
	card->irq_rising = 0xff;
	card->irq_falling = 0;
	card->irq_masked = 0;

	/* Reset any interrupts that may have still been pending in the
	 * device.
	 */
	iowrite32(0xff, card->control_regs + PCIE1761H_OFFSET_IRQFLAG);

	/* Disable the debounce logic, so that we are in a well-defined
	 * state every time we start up.
	 */
	iowrite8(0,
		 card->control_regs + PCIE1761H_OFFSET_DICTL_DEBOUNCE_ENABLE);
	iowrite8(0, card->control_regs + PCIE1761H_OFFSET_DICTL_DEBOUNCE_TIME);

	/* Don't reset the DO states here, because the card has a dip
	 * switch that can be used to tell it to remember the last DO
	 * state upon powering on; if the user selected that mode we
	 * don't want to blindly reset that state.
	 */

	err = devm_gpiochip_add_data(dev, &card->chip, card);
	if (err < 0) {
		dev_err(dev, "can't initialize gpiochip\n");
		return err;
	}

	err = devm_request_irq(dev, pdev->irq, adv_pcie_1761h_irq_handler,
			       IRQF_SHARED, KBUILD_MODNAME, card);
	if (err) {
		dev_err(dev, "can't set up interrupt handler\n");
		return err;
	}
	return 0;
}

static struct pci_driver adv_pcie_1761h_driver = {
	.name = "gpio-adv-pcie-1761h",
	.id_table = adv_pcie_1761h_ids,
	.probe = adv_pcie_1761h_probe,
};

module_pci_driver(adv_pcie_1761h_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Christian Seiler <c.seiler@luxflux.de>");
MODULE_DESCRIPTION("Advantech PCIe-1761H GPIO driver");
