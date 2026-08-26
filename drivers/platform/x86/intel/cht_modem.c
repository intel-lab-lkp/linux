// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel Cherry Trail ACPI modem power driver
 *
 * Copyright (C) 2008, 2013 Intel Corporation
 * Copyright (C) 2026 Maurizio Casciano
 */

#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/usb.h>
#include <linux/uuid.h>
#include <linux/workqueue.h>

#define CHT_MODEM_DSM_REVISION		0
#define CHT_MODEM_DSM_POWER_OFF		1
#define CHT_MODEM_DSM_RESET		3

/* ACPI's MCD0001 PMIC package for the XMM7260_CONF_3 configuration. */
#define CHT_MODEM_PMIC_HID		"INT34D3"
#define CHT_MODEM_PMIC_CTRL_REG		0x6e29
#define CHT_MODEM_PMIC_CTRL_MASK	GENMASK(1, 0)
#define CHT_MODEM_PMIC_CTRL_ON		BIT(0)
#define CHT_MODEM_PMIC_POWER_DELAY_US	20000

#define PCI_DEVICE_ID_INTEL_CHT_XHCI	0x22b5
#define CHT_MODEM_USB_VENDOR_ID		0x8087
#define CHT_MODEM_USB_BOOT_PRODUCT_ID	0x07ef
#define CHT_MODEM_USB_MBIM_PRODUCT_ID	0x0911
#define CHT_MODEM_STATUS_TRIGGER_DELAY	(50 * HZ)
#define CHT_MODEM_STATUS_RETRY_DELAY	(5 * HZ)
#define CHT_MODEM_ENUMERATION_TIMEOUT	(90 * HZ)

static const guid_t cht_modem_dsm_guid =
	GUID_INIT(0xac340cb7, 0xe901, 0x45bf,
		  0xb7, 0xe6, 0x2b, 0x34, 0xec, 0x93, 0x1e, 0x23);

struct cht_modem {
	struct device *dev;
	struct regmap *pmic_regmap;
	struct pci_dev *xhci;
	struct delayed_work trigger_status_work;
	struct delayed_work release_xhci_work;
	/* Protects powered and serializes firmware operations. */
	struct mutex lock;
	bool xhci_runtime_held;
	bool powered;
};

struct cht_modem_usb_state {
	bool boot;
	bool runtime;
	int status_ret;
};

static const struct dmi_system_id cht_modem_dmi_table[] = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Lenovo YB1-X91L"),
		},
	},
	{ }
};

static int cht_modem_evaluate_dsm(struct cht_modem *modem, u64 function)
{
	union acpi_object params[4] = {
		{
			.buffer = {
				.type = ACPI_TYPE_BUFFER,
				.length = sizeof(cht_modem_dsm_guid),
				.pointer = (u8 *)&cht_modem_dsm_guid,
			},
		},
		{
			.integer = {
				.type = ACPI_TYPE_INTEGER,
				.value = CHT_MODEM_DSM_REVISION,
			},
		},
		{
			.integer = {
				.type = ACPI_TYPE_INTEGER,
				.value = function,
			},
		},
		{
			.package = {
				.type = ACPI_TYPE_PACKAGE,
				.count = 0,
				.elements = NULL,
			},
		},
	};
	struct acpi_object_list input = {
		.count = ARRAY_SIZE(params),
		.pointer = params,
	};
	acpi_status status;

	/* These firmware functions perform an action without returning data. */
	status = acpi_evaluate_object(ACPI_HANDLE(modem->dev), "_DSM", &input,
				      NULL);
	if (ACPI_FAILURE(status)) {
		dev_err(modem->dev, "_DSM function %llu failed: %s\n", function,
			acpi_format_exception(status));
		return -EIO;
	}

	return 0;
}

static int cht_modem_set_pmic_power(struct cht_modem *modem, bool on)
{
	unsigned int value = on ? CHT_MODEM_PMIC_CTRL_ON : 0;
	int ret;

	ret = regmap_update_bits(modem->pmic_regmap, CHT_MODEM_PMIC_CTRL_REG,
				 CHT_MODEM_PMIC_CTRL_MASK, value);
	if (ret)
		dev_err(modem->dev, "failed to set modem PMIC power: %d\n", ret);

	return ret;
}

static void cht_modem_release_xhci_runtime(struct cht_modem *modem)
{
	if (!modem->xhci_runtime_held)
		return;

	pm_runtime_put(&modem->xhci->dev);
	modem->xhci_runtime_held = false;
	dev_info(modem->dev,
		 "released xHCI runtime hold after modem enumeration window\n");
}

static int cht_modem_check_usb_device(struct usb_device *udev, void *data)
{
	struct cht_modem_usb_state *state = data;
	u16 product;
	u16 status;

	if (le16_to_cpu(udev->descriptor.idVendor) !=
	    CHT_MODEM_USB_VENDOR_ID)
		return 0;

	product = le16_to_cpu(udev->descriptor.idProduct);
	if (product == CHT_MODEM_USB_MBIM_PRODUCT_ID) {
		state->runtime = true;
		return 1;
	}

	if (product != CHT_MODEM_USB_BOOT_PRODUCT_ID)
		return 0;

	state->boot = true;
	state->status_ret = usb_get_std_status(udev, USB_RECIP_DEVICE, 0,
					       &status);
	return 1;
}

static void cht_modem_trigger_status_work(struct work_struct *work)
{
	struct cht_modem *modem =
		container_of(to_delayed_work(work), struct cht_modem,
			     trigger_status_work);
	struct cht_modem_usb_state state = { };

	mutex_lock(&modem->lock);
	if (!modem->powered || !modem->xhci_runtime_held)
		goto out;

	usb_for_each_dev(&state, cht_modem_check_usb_device);
	if (state.runtime) {
		dev_info(modem->dev, "XMM7260 MBIM runtime interface detected\n");
		goto out;
	}

	if (state.boot) {
		if (state.status_ret && state.status_ret != -ENODEV &&
		    state.status_ret != -ESHUTDOWN)
			dev_warn(modem->dev,
				 "XMM7260 boot-interface GET_STATUS failed: %d\n",
				 state.status_ret);
		else
			dev_info(modem->dev,
				 "triggered XMM7260 boot-interface GET_STATUS\n");
	}

	/* Retry until the runtime interface appears or the hold expires. */
	mod_delayed_work(system_dfl_wq, &modem->trigger_status_work,
			 CHT_MODEM_STATUS_RETRY_DELAY);
out:
	mutex_unlock(&modem->lock);
}

static void cht_modem_release_xhci_work(struct work_struct *work)
{
	struct cht_modem *modem =
		container_of(to_delayed_work(work), struct cht_modem,
			     release_xhci_work);

	cancel_delayed_work(&modem->trigger_status_work);
	mutex_lock(&modem->lock);
	cht_modem_release_xhci_runtime(modem);
	mutex_unlock(&modem->lock);
}

static int cht_modem_hold_xhci_runtime(struct cht_modem *modem)
{
	modem->xhci = pci_get_device(PCI_VENDOR_ID_INTEL,
				     PCI_DEVICE_ID_INTEL_CHT_XHCI, NULL);
	if (!modem->xhci)
		return -EPROBE_DEFER;

	/*
	 * The XMM7260 first enumerates as 8087:07ef and takes roughly 48
	 * seconds to re-enumerate as the 8087:0911 MBIM modem. Keep xHCI in
	 * D0 across that window; otherwise SSIC link training stops in RxDetect.
	 */
	pm_runtime_get_noresume(&modem->xhci->dev);
	modem->xhci_runtime_held = true;
	mod_delayed_work(system_dfl_wq, &modem->release_xhci_work,
			 CHT_MODEM_ENUMERATION_TIMEOUT);

	return 0;
}

static struct regmap *cht_modem_get_pmic_regmap(struct device *dev)
{
	struct acpi_device *adev;
	struct device *pmic_dev;
	struct regmap *regmap;

	adev = acpi_dev_get_first_match_dev(CHT_MODEM_PMIC_HID, NULL, -1);
	if (!adev)
		return ERR_PTR(-EPROBE_DEFER);

	pmic_dev = get_device(acpi_get_first_physical_node(adev));
	acpi_dev_put(adev);
	if (!pmic_dev)
		return ERR_PTR(-EPROBE_DEFER);

	regmap = dev_get_regmap(pmic_dev, NULL);
	if (!regmap) {
		put_device(pmic_dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	if (!device_link_add(dev, pmic_dev, DL_FLAG_AUTOREMOVE_CONSUMER)) {
		put_device(pmic_dev);
		return ERR_PTR(-ENOMEM);
	}

	put_device(pmic_dev);
	return regmap;
}

static int cht_modem_power_on(struct cht_modem *modem)
{
	int ret = 0;

	mutex_lock(&modem->lock);
	if (!modem->powered) {
		ret = cht_modem_set_pmic_power(modem, true);
		if (ret)
			goto out;

		usleep_range(CHT_MODEM_PMIC_POWER_DELAY_US,
			     CHT_MODEM_PMIC_POWER_DELAY_US + 1000);
		/* MRST also cycles the SSIC pull-down/pull-up state around MDON. */
		ret = cht_modem_evaluate_dsm(modem, CHT_MODEM_DSM_RESET);
		if (!ret) {
			modem->powered = true;
			dev_info(modem->dev,
				 "powered on and holding xHCI for SSIC enumeration\n");
		} else {
			cht_modem_set_pmic_power(modem, false);
		}
	}

out:
	mutex_unlock(&modem->lock);

	return ret;
}

static void cht_modem_power_off(struct cht_modem *modem)
{
	int ret;

	mutex_lock(&modem->lock);
	if (modem->powered) {
		ret = cht_modem_evaluate_dsm(modem, CHT_MODEM_DSM_POWER_OFF);
		if (!ret)
			ret = cht_modem_set_pmic_power(modem, false);
		if (!ret)
			modem->powered = false;
	}
	mutex_unlock(&modem->lock);
}

static int cht_modem_probe(struct platform_device *pdev)
{
	struct cht_modem *modem;
	int ret;

	if (!dmi_check_system(cht_modem_dmi_table))
		return -ENODEV;

	/*
	 * INT34D0 advertises functions 0 and 1 only, despite also implementing
	 * the power-on function used below.
	 */
	if (!acpi_check_dsm(ACPI_HANDLE(&pdev->dev), &cht_modem_dsm_guid,
			    CHT_MODEM_DSM_REVISION,
			    BIT(CHT_MODEM_DSM_POWER_OFF)))
		return -ENODEV;

	modem = devm_kzalloc(&pdev->dev, sizeof(*modem), GFP_KERNEL);
	if (!modem)
		return -ENOMEM;

	modem->dev = &pdev->dev;
	modem->pmic_regmap = cht_modem_get_pmic_regmap(&pdev->dev);
	if (IS_ERR(modem->pmic_regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(modem->pmic_regmap),
				     "failed to get Whiskey Cove PMIC regmap\n");

	mutex_init(&modem->lock);
	INIT_DELAYED_WORK(&modem->release_xhci_work,
			  cht_modem_release_xhci_work);
	INIT_DELAYED_WORK(&modem->trigger_status_work,
			  cht_modem_trigger_status_work);
	platform_set_drvdata(pdev, modem);

	ret = cht_modem_hold_xhci_runtime(modem);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to hold Cherry Trail xHCI runtime PM\n");

	ret = cht_modem_power_on(modem);
	if (ret) {
		cancel_delayed_work_sync(&modem->release_xhci_work);
		cht_modem_release_xhci_runtime(modem);
		pci_dev_put(modem->xhci);
		modem->xhci = NULL;
	} else {
		mod_delayed_work(system_dfl_wq, &modem->trigger_status_work,
				 CHT_MODEM_STATUS_TRIGGER_DELAY);
	}

	return ret;
}

static void cht_modem_remove(struct platform_device *pdev)
{
	struct cht_modem *modem = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&modem->trigger_status_work);
	cancel_delayed_work_sync(&modem->release_xhci_work);
	mutex_lock(&modem->lock);
	cht_modem_release_xhci_runtime(modem);
	mutex_unlock(&modem->lock);
	cht_modem_power_off(modem);
	pci_dev_put(modem->xhci);
}

static void cht_modem_shutdown(struct platform_device *pdev)
{
	struct cht_modem *modem = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&modem->trigger_status_work);
	cancel_delayed_work_sync(&modem->release_xhci_work);
	cht_modem_power_off(modem);
}

static const struct acpi_device_id cht_modem_acpi_ids[] = {
	{ "INT34D0" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, cht_modem_acpi_ids);

static struct platform_driver cht_modem_driver = {
	.driver = {
		.name = "intel-cht-modem",
		.acpi_match_table = cht_modem_acpi_ids,
	},
	.probe = cht_modem_probe,
	.remove = cht_modem_remove,
	.shutdown = cht_modem_shutdown,
};

static int __init cht_modem_init(void)
{
	return platform_driver_register(&cht_modem_driver);
}
subsys_initcall(cht_modem_init);

static void __exit cht_modem_exit(void)
{
	platform_driver_unregister(&cht_modem_driver);
}
module_exit(cht_modem_exit);

MODULE_DESCRIPTION("Intel Cherry Trail ACPI modem power driver");
MODULE_LICENSE("GPL");
