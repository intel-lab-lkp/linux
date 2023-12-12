// SPDX-License-Identifier: GPL-2.0
//
// CS35L41 ALSA HDA Property driver
//
// Copyright 2023 Cirrus Logic, Inc.
//
// Author: Stefan Binding <sbinding@opensource.cirrus.com>

#include <linux/gpio/consumer.h>
#include <linux/spi/spi.h>
#include <linux/string.h>
#include "cs35l41_hda_property.h"

/*
 * Device 10280BEB (Dell XPS 9530) doesn't have _DSD at all. Moreover, pin that is typically
 * used for `speaker_id` is missing. SPI's cs-gpios definitions are also missing.
 */
static int dell_fiorano_no_acpi(struct cs35l41_hda *cs35l41, struct device *physdev, int id,
				const char *hid)
{
	struct cs35l41_hw_cfg *hw_cfg = &cs35l41->hw_cfg;
	struct spi_device *spi = to_spi_device(cs35l41->dev);

	/*
	 * 10280BEB has a firmware bug, which wrongly enables clock divider for intel-lpss
	 * Resultant SPI clock is 100Mhz/32767=3051Hz, which leads to ~3 minute hang on boot/wake up
	 * Avoid initializing device if lpss was not patched/fixed UEFI was not installed
	 */
	if (spi->max_speed_hz < CS35L41_SPI_MAX_FREQ) {
		dev_err(cs35l41->dev, "SPI's max_speed_hz is capped at %u Hz, will not continue to avoid hanging\n",
			spi->max_speed_hz);
		return -EINVAL;
	}

	dev_info(cs35l41->dev, "Adding DSD properties for %s\n", cs35l41->acpi_subsystem_id);

	/* check SPI address to assign the index */
	cs35l41->index = id;
	cs35l41->channel_index = 0;
	/* 10280BEB is missing pin which is typically assigned to `spk-id-gpios` */
	cs35l41->speaker_id = cs35l41_get_speaker_id(physdev, cs35l41->index, 2, -1);
	cs35l41->reset_gpio = gpiod_get_index(physdev, NULL, 1, GPIOD_OUT_LOW);

	hw_cfg->spk_pos = cs35l41->index  ? 1 : 0;	// 0th L, 1st R
	hw_cfg->bst_type = CS35L41_EXT_BOOST;
	hw_cfg->gpio1.func = CS35l41_VSPK_SWITCH;
	hw_cfg->gpio1.valid = true;
	hw_cfg->gpio2.func = CS35L41_INTERRUPT;
	hw_cfg->gpio2.valid = true;
	hw_cfg->valid = true;

	/* Add second cs-gpio here */
	if (cs35l41->index)
		spi->cs_gpiod = gpiod_get_index(physdev, NULL, 0, GPIOD_OUT_HIGH);

	return 0;
}

/*
 * Device CLSA010(0/1) doesn't have _DSD so a gpiod_get by the label reset won't work.
 * And devices created by serial-multi-instantiate don't have their device struct
 * pointing to the correct fwnode, so acpi_dev must be used here.
 * And devm functions expect that the device requesting the resource has the correct
 * fwnode.
 */
static int lenovo_legion_no_acpi(struct cs35l41_hda *cs35l41, struct device *physdev, int id,
				 const char *hid)
{
	struct cs35l41_hw_cfg *hw_cfg = &cs35l41->hw_cfg;

	/* check I2C address to assign the index */
	cs35l41->index = id == 0x40 ? 0 : 1;
	cs35l41->channel_index = 0;
	cs35l41->reset_gpio = gpiod_get_index(physdev, NULL, 0, GPIOD_OUT_HIGH);
	cs35l41->speaker_id = cs35l41_get_speaker_id(physdev, 0, 0, 2);
	hw_cfg->spk_pos = cs35l41->index;
	hw_cfg->gpio2.func = CS35L41_INTERRUPT;
	hw_cfg->gpio2.valid = true;
	hw_cfg->valid = true;

	if (strcmp(hid, "CLSA0100") == 0) {
		hw_cfg->bst_type = CS35L41_EXT_BOOST_NO_VSPK_SWITCH;
	} else if (strcmp(hid, "CLSA0101") == 0) {
		hw_cfg->bst_type = CS35L41_EXT_BOOST;
		hw_cfg->gpio1.func = CS35l41_VSPK_SWITCH;
		hw_cfg->gpio1.valid = true;
	}

	return 0;
}

/*
 * Device 103C89C6 does have _DSD, however it is setup to use the wrong boost type.
 * We can override the _DSD to correct the boost type here.
 * Since this laptop has valid ACPI, we do not need to handle cs-gpios, since that already exists
 * in the ACPI.
 */
static int hp_vision_acpi_fix(struct cs35l41_hda *cs35l41, struct device *physdev, int id,
			      const char *hid)
{
	struct cs35l41_hw_cfg *hw_cfg = &cs35l41->hw_cfg;

	dev_info(cs35l41->dev, "Adding DSD properties for %s\n", cs35l41->acpi_subsystem_id);

	cs35l41->index = id;
	cs35l41->channel_index = 0;

	/*
	 * This system has _DSD, it just contains an error, so we can still get the reset using
	 * the "reset" label.
	 */
	cs35l41->reset_gpio = fwnode_gpiod_get_index(acpi_fwnode_handle(cs35l41->dacpi), "reset",
						     cs35l41->index, GPIOD_OUT_LOW,
						     "cs35l41-reset");
	cs35l41->speaker_id = -ENOENT;
	hw_cfg->spk_pos = cs35l41->index ? 0 : 1; // right:left
	hw_cfg->gpio1.func = CS35L41_NOT_USED;
	hw_cfg->gpio1.valid = true;
	hw_cfg->gpio2.func = CS35L41_INTERRUPT;
	hw_cfg->gpio2.valid = true;
	hw_cfg->bst_type = CS35L41_INT_BOOST;
	hw_cfg->bst_ind = 1000;
	hw_cfg->bst_ipk = 4500;
	hw_cfg->bst_cap = 24;
	hw_cfg->valid = true;

	return 0;
}

struct cs35l41_prop_model {
	const char *hid;
	const char *ssid;
	int (*add_prop)(struct cs35l41_hda *cs35l41, struct device *physdev, int id,
			const char *hid);
};

static const struct cs35l41_prop_model cs35l41_prop_model_table[] = {
	{ "CLSA0100", NULL, lenovo_legion_no_acpi },
	{ "CLSA0101", NULL, lenovo_legion_no_acpi },
	{ "CSC3551", "103C89C6", hp_vision_acpi_fix },
	{ "CSC3551", "10280BEB", dell_fiorano_no_acpi },
	{}
};

int cs35l41_add_dsd_properties(struct cs35l41_hda *cs35l41, struct device *physdev, int id,
			       const char *hid)
{
	const struct cs35l41_prop_model *model;

	for (model = cs35l41_prop_model_table; model->hid; model++) {
		if (!strcmp(model->hid, hid) &&
		    (!model->ssid ||
		     (cs35l41->acpi_subsystem_id &&
		      !strcmp(model->ssid, cs35l41->acpi_subsystem_id))))
			return model->add_prop(cs35l41, physdev, id, hid);
	}

	return -ENOENT;
}
