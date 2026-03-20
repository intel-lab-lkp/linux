// SPDX-License-Identifier: GPL-2.0
/* Author: Dan Scally <djrscally@gmail.com> */

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/unaligned.h>
#include <linux/mfd/core.h>
#include <linux/mfd/tps68470.h>
#include <linux/platform_device.h>
#include <linux/platform_data/tps68470.h>
#include <linux/platform_data/x86/int3472.h>
#include <linux/regmap.h>
#include <linux/string.h>

#include "tps68470.h"

#define DESIGNED_FOR_CHROMEOS		1
#define DESIGNED_FOR_WINDOWS		2

#define TPS68470_WIN_MFD_CELL_COUNT	3

static const struct mfd_cell tps68470_cros[] = {
	{ .name = "tps68470-gpio" },
	{ .name = "tps68470_pmic_opregion" },
};

static const struct regmap_config tps68470_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = TPS68470_REG_MAX,
};

static int tps68470_chip_init(struct device *dev, struct regmap *regmap)
{
	unsigned int version;
	int ret;

	/* Force software reset */
	ret = regmap_write(regmap, TPS68470_REG_RESET, TPS68470_REG_RESET_MASK);
	if (ret)
		return ret;

	ret = regmap_read(regmap, TPS68470_REG_REVID, &version);
	if (ret) {
		dev_err(dev, "Failed to read revision register: %d\n", ret);
		return ret;
	}

	dev_info(dev, "TPS68470 REVID: 0x%02x\n", version);

	return 0;
}

/** skl_int3472_tps68470_calc_type: Check what platform a device is designed for
 * @adev: A pointer to a &struct acpi_device
 *
 * Check CLDB buffer against the PMIC's adev. If present, then we check
 * the value of control_logic_type field and follow one of the
 * following scenarios:
 *
 *	1. No CLDB - likely ACPI tables designed for ChromeOS. We
 *	create platform devices for the GPIOs and OpRegion drivers.
 *
 *	2. CLDB, with control_logic_type = 2 - probably ACPI tables
 *	made for Windows 2-in-1 platforms. Register pdevs for GPIO,
 *	Clock and Regulator drivers to bind to.
 *
 *	3. Any other value in control_logic_type, we should never have
 *	gotten to this point; fail probe and return.
 *
 * Return:
 * * 1		Device intended for ChromeOS
 * * 2		Device intended for Windows
 * * -EINVAL	Where @adev has an object named CLDB but it does not conform to
 *		our expectations
 */
static int skl_int3472_tps68470_calc_type(struct acpi_device *adev)
{
	struct int3472_cldb cldb = { 0 };
	int ret;

	/*
	 * A CLDB buffer that exists, but which does not match our expectations
	 * should trigger an error so we don't blindly continue.
	 */
	ret = skl_int3472_fill_cldb(adev, &cldb);
	if (ret && ret != -ENODEV)
		return ret;

	if (ret)
		return DESIGNED_FOR_CHROMEOS;

	if (cldb.control_logic_type != 2)
		return -EINVAL;

	return DESIGNED_FOR_WINDOWS;
}

/*
 * Return the size of the flexible array member, because we'll need that later
 * on to pass .pdata_size to cells.
 */
static int
skl_int3472_fill_clk_pdata(struct device *dev, struct tps68470_clk_platform_data **clk_pdata)
{
	struct acpi_device *adev = ACPI_COMPANION(dev);
	struct acpi_device *consumer;
	unsigned int n_consumers = 0;
	const char *sensor_name;
	unsigned int i = 0;

	for_each_acpi_consumer_dev(adev, consumer)
		n_consumers++;

	if (!n_consumers) {
		dev_err(dev, "INT3472 seems to have no dependents\n");
		return -ENODEV;
	}

	*clk_pdata = devm_kzalloc(dev, struct_size(*clk_pdata, consumers, n_consumers),
				  GFP_KERNEL);
	if (!*clk_pdata)
		return -ENOMEM;

	(*clk_pdata)->n_consumers = n_consumers;
	i = 0;

	for_each_acpi_consumer_dev(adev, consumer) {
		sensor_name = devm_kasprintf(dev, GFP_KERNEL, I2C_DEV_NAME_FORMAT,
					     acpi_dev_name(consumer));
		if (!sensor_name) {
			acpi_dev_put(consumer);
			return -ENOMEM;
		}

		(*clk_pdata)->consumers[i].consumer_dev_name = sensor_name;
		i++;
	}

	return n_consumers;
}

/* Dell Latitude 5285 GNVS fix
 *
 * The BIOS leaves GNVS fields C0TP, L0CL and L1CL at zero after POST.
 * With C0TP=0 the ACPI _DEP on INT3479 resolves to PCI0 instead of CLP0
 * (INT3472), so ipu_bridge never creates i2c-INT3479:00 (OV5670 front cam).
 * With L0CL=L1CL=0 the TPS68470 clock driver disables all clock outputs,
 * making both sensors unreachable over I2C.
 *
 * Fix: at TPS68470 probe time, locate the GNVS SystemMemory OperationRegion
 * by scanning the DSDT/SSDTs for its AML definition, map the region, and
 * write 0x02 (19.2 MHz) into C0TP, L0CL and L1CL.
 *
 * Field byte offsets (verified from DSDT disassembly, GNVS size 0x0725):
 *   C0TP: 0x43A   L0CL: 0x4F7   L1CL: 0x549
 */
#define DELL5285_C0TP_OFF	0x43A
#define DELL5285_L0CL_OFF	0x4F7
#define DELL5285_L1CL_OFF	0x549
/* Minimum GNVS region size: last field (L1CL) is 1 byte at 0x549 */
#define DELL5285_GNVS_MIN_SIZE	(DELL5285_L1CL_OFF + 1)

/* AML integer opcodes (ACPI 6.4, §20.2.3) */
#define AML_ZERO_OP		0x00
#define AML_ONE_OP		0x01
#define AML_BYTE_PREFIX		0x0A
#define AML_WORD_PREFIX		0x0B
#define AML_DWORD_PREFIX	0x0C
#define AML_QWORD_PREFIX	0x0E

/**
 * aml_parse_int - Parse one AML integer at @p, store value in @val.
 * Returns number of bytes consumed, or 0 on failure.
 */
static int aml_parse_int(const u8 *p, const u8 *end, u64 *val)
{
	if (p >= end)
		return 0;
	switch (*p) {
	case AML_ZERO_OP:  *val = 0; return 1;
	case AML_ONE_OP:   *val = 1; return 1;
	case AML_BYTE_PREFIX:
		if (p + 2 > end) return 0;
		*val = p[1]; return 2;
	case AML_WORD_PREFIX:
		if (p + 3 > end) return 0;
		*val = get_unaligned_le16(p + 1); return 3;
	case AML_DWORD_PREFIX:
		if (p + 5 > end) return 0;
		*val = get_unaligned_le32(p + 1); return 5;
	case AML_QWORD_PREFIX:
		if (p + 9 > end) return 0;
		*val = get_unaligned_le64(p + 1); return 9;
	}
	return 0;
}

/**
 * dell5285_gnvs_from_table - Scan one ACPI table for the GNVS OperationRegion.
 *
 * Searches the AML body of @tbl for the byte sequence:
 *   ExtOp(0x5B) OpRegionOp(0x80) NameSeg("GNVS") RegionSpace(SystemMemory=0x00)
 * followed by two AML integers (region address and length).
 *
 * Returns true and fills @addr / @size if found and plausible.
 */
static bool dell5285_gnvs_from_table(const struct acpi_table_header *tbl,
				     phys_addr_t *addr, u32 *size)
{
	/* AML: ExtOp OpRegionOp NameSeg("GNVS") SystemMemory */
	static const u8 sig[] = { 0x5B, 0x80, 'G', 'N', 'V', 'S', 0x00 };
	const u8 *aml = (const u8 *)tbl + sizeof(*tbl);
	const u8 *end = (const u8 *)tbl + tbl->length;
	const u8 *p;

	for (p = aml; p + sizeof(sig) < end; p++) {
		u64 region_addr, region_size;
		int consumed;

		if (memcmp(p, sig, sizeof(sig)) != 0)
			continue;

		p += sizeof(sig);
		consumed = aml_parse_int(p, end, &region_addr);
		if (!consumed || !region_addr)
			continue;

		p += consumed;
		consumed = aml_parse_int(p, end, &region_size);
		if (!consumed || region_size < DELL5285_GNVS_MIN_SIZE)
			continue;

		*addr = (phys_addr_t)region_addr;
		*size = (u32)region_size;
		return true;
	}
	return false;
}

/**
 * dell5285_gnvs_find - Locate the GNVS OperationRegion address by scanning
 *                      DSDT and SSDTs.
 */
static bool dell5285_gnvs_find(phys_addr_t *addr, u32 *size)
{
	struct acpi_table_header *tbl;
	u32 i;

	/* DSDT */
	if (ACPI_SUCCESS(acpi_get_table(ACPI_SIG_DSDT, 1, &tbl))) {
		bool found = dell5285_gnvs_from_table(tbl, addr, size);

		acpi_put_table(tbl);
		if (found)
			return true;
	}

	/* SSDTs (instance numbers start at 1, stop at first failure) */
	for (i = 1; i <= 32; i++) {
		bool found;

		if (ACPI_FAILURE(acpi_get_table(ACPI_SIG_SSDT, i, &tbl)))
			break;
		found = dell5285_gnvs_from_table(tbl, addr, size);
		acpi_put_table(tbl);
		if (found)
			return true;
	}

	return false;
}

static const struct dmi_system_id dell5285_gnvs_dmi[] = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Latitude 5285"),
		},
	},
	{ }
};

static void dell5285_gnvs_fix(void)
{
	phys_addr_t gnvs_addr;
	u32 gnvs_size;
	void *gnvs;

	if (!dmi_check_system(dell5285_gnvs_dmi))
		return;

	if (!dell5285_gnvs_find(&gnvs_addr, &gnvs_size)) {
		pr_err("int3472-tps68470: Dell 5285: GNVS OperationRegion not found in DSDT/SSDTs\n");
		return;
	}

	gnvs = acpi_os_map_memory(gnvs_addr, gnvs_size);
	if (!gnvs) {
		pr_err("int3472-tps68470: Dell 5285: failed to map GNVS at %pa\n",
		       &gnvs_addr);
		return;
	}

	pr_info("int3472-tps68470: Dell 5285 GNVS fix at %pa: C0TP=0x%02x L0CL=0x%02x L1CL=0x%02x -> 0x02\n",
		&gnvs_addr,
		*(u8 *)(gnvs + DELL5285_C0TP_OFF),
		*(u8 *)(gnvs + DELL5285_L0CL_OFF),
		*(u8 *)(gnvs + DELL5285_L1CL_OFF));

	*(u8 *)(gnvs + DELL5285_C0TP_OFF) = 0x02;
	*(u8 *)(gnvs + DELL5285_L0CL_OFF) = 0x02;
	*(u8 *)(gnvs + DELL5285_L1CL_OFF) = 0x02;

	acpi_os_unmap_memory(gnvs, gnvs_size);
}

static int skl_int3472_tps68470_probe(struct i2c_client *client)
{
	struct acpi_device *adev = ACPI_COMPANION(&client->dev);
	const struct int3472_tps68470_board_data *board_data;
	struct tps68470_clk_platform_data *clk_pdata;
	struct mfd_cell *cells;
	struct regmap *regmap;
	int n_consumers;
	int device_type;
	int ret;
	int i;

	if (!adev)
		return -ENODEV;

	dell5285_gnvs_fix();

	n_consumers = skl_int3472_fill_clk_pdata(&client->dev, &clk_pdata);
	if (n_consumers < 0)
		return n_consumers;

	regmap = devm_regmap_init_i2c(client, &tps68470_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&client->dev, "Failed to create regmap: %ld\n", PTR_ERR(regmap));
		return PTR_ERR(regmap);
	}

	i2c_set_clientdata(client, regmap);

	ret = tps68470_chip_init(&client->dev, regmap);
	if (ret < 0) {
		dev_err(&client->dev, "TPS68470 init error %d\n", ret);
		return ret;
	}

	device_type = skl_int3472_tps68470_calc_type(adev);
	switch (device_type) {
	case DESIGNED_FOR_WINDOWS:
		board_data = int3472_tps68470_get_board_data(dev_name(&client->dev));
		if (!board_data)
			return dev_err_probe(&client->dev, -ENODEV, "No board-data found for this model\n");

		cells = kzalloc_objs(*cells, TPS68470_WIN_MFD_CELL_COUNT);
		if (!cells)
			return -ENOMEM;

		/*
		 * The order of the cells matters here! The clk must be first
		 * because the regulator depends on it. The gpios must be last,
		 * acpi_gpiochip_add() calls acpi_dev_clear_dependencies() and
		 * the clk + regulators must be ready when this happens.
		 */
		cells[0].name = "tps68470-clk";
		cells[0].platform_data = clk_pdata;
		cells[0].pdata_size = struct_size(clk_pdata, consumers, n_consumers);
		cells[1].name = "tps68470-regulator";
		cells[1].platform_data = (void *)board_data->tps68470_regulator_pdata;
		cells[1].pdata_size = sizeof(struct tps68470_regulator_platform_data);
		cells[2].name = "tps68470-gpio";

		for (i = 0; i < board_data->n_gpiod_lookups; i++)
			gpiod_add_lookup_table(board_data->tps68470_gpio_lookup_tables[i]);

		ret = devm_mfd_add_devices(&client->dev, PLATFORM_DEVID_NONE,
					   cells, TPS68470_WIN_MFD_CELL_COUNT,
					   NULL, 0, NULL);
		kfree(cells);

		if (ret) {
			for (i = 0; i < board_data->n_gpiod_lookups; i++)
				gpiod_remove_lookup_table(board_data->tps68470_gpio_lookup_tables[i]);
		}

		break;
	case DESIGNED_FOR_CHROMEOS:
		ret = devm_mfd_add_devices(&client->dev, PLATFORM_DEVID_NONE,
					   tps68470_cros, ARRAY_SIZE(tps68470_cros),
					   NULL, 0, NULL);
		break;
	default:
		dev_err(&client->dev, "Failed to add MFD devices\n");
		return device_type;
	}

	/*
	 * No acpi_dev_clear_dependencies() here, since the acpi_gpiochip_add()
	 * for the GPIO cell already does this.
	 */

	return ret;
}

static void skl_int3472_tps68470_remove(struct i2c_client *client)
{
	const struct int3472_tps68470_board_data *board_data;
	int i;

	board_data = int3472_tps68470_get_board_data(dev_name(&client->dev));
	if (board_data) {
		for (i = 0; i < board_data->n_gpiod_lookups; i++)
			gpiod_remove_lookup_table(board_data->tps68470_gpio_lookup_tables[i]);
	}
}

static const struct acpi_device_id int3472_device_id[] = {
	{ "INT3472", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, int3472_device_id);

static struct i2c_driver int3472_tps68470 = {
	.driver = {
		.name = "int3472-tps68470",
		.acpi_match_table = int3472_device_id,
	},
	.probe = skl_int3472_tps68470_probe,
	.remove = skl_int3472_tps68470_remove,
};
module_i2c_driver(int3472_tps68470);

MODULE_DESCRIPTION("Intel SkyLake INT3472 ACPI TPS68470 Device Driver");
MODULE_AUTHOR("Daniel Scally <djrscally@gmail.com>");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS("INTEL_INT3472");
MODULE_SOFTDEP("pre: clk-tps68470 tps68470-regulator");
