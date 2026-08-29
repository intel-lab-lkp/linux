// SPDX-License-Identifier: GPL-2.0+
/*
 * Airoha Ethernet PHY common library
 *
 * Copyright (C) 2026 Airoha Technology Corp.
 * Copyright (C) 2026 Collabora Ltd.
 *                    Louis-Alexis Eyraud <louisalexis.eyraud@collabora.com>
 */

#include <linux/export.h>
#include <linux/firmware.h>
#include <linux/iopoll.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/phy.h>
#include <linux/unaligned.h>
#include <linux/wordpart.h>

#include "air_phy_lib.h"
#include "phylib.h"

static int __air_buckpbus_reg_read(struct mii_bus *bus, int addr,
				   u32 pbus_address, u32 *pbus_data)
{
	int pbus_data_low, pbus_data_high;
	int ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_MODE,
			      AIR_BPBUS_MODE_ADDR_FIXED);
	if (ret < 0)
		return ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_RD_ADDR_HIGH,
			      upper_16_bits(pbus_address));
	if (ret < 0)
		return ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_RD_ADDR_LOW,
			      lower_16_bits(pbus_address));
	if (ret < 0)
		return ret;

	pbus_data_high = __mdiobus_read(bus, addr, AIR_BPBUS_RD_DATA_HIGH);
	if (pbus_data_high < 0)
		return pbus_data_high;

	pbus_data_low = __mdiobus_read(bus, addr, AIR_BPBUS_RD_DATA_LOW);
	if (pbus_data_low < 0)
		return pbus_data_low;

	*pbus_data = pbus_data_low | (pbus_data_high << 16);
	return 0;
}

static int __air_buckpbus_reg_write(struct mii_bus *bus, int addr,
				    u32 pbus_address, u32 pbus_data)
{
	int ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_MODE,
			      AIR_BPBUS_MODE_ADDR_FIXED);
	if (ret < 0)
		return ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_WR_ADDR_HIGH,
			      upper_16_bits(pbus_address));
	if (ret < 0)
		return ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_WR_ADDR_LOW,
			      lower_16_bits(pbus_address));
	if (ret < 0)
		return ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_WR_DATA_HIGH,
			      upper_16_bits(pbus_data));
	if (ret < 0)
		return ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_WR_DATA_LOW,
			      lower_16_bits(pbus_data));
	if (ret < 0)
		return ret;

	return 0;
}

static int __air_buckpbus_reg_modify(struct mii_bus *bus, int addr,
				     u32 pbus_address, u32 mask, u32 set)
{
	int pbus_data_low, pbus_data_high;
	u32 pbus_data_old, pbus_data_new;
	int ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_MODE,
			      AIR_BPBUS_MODE_ADDR_FIXED);
	if (ret < 0)
		return ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_RD_ADDR_HIGH,
			      upper_16_bits(pbus_address));
	if (ret < 0)
		return ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_RD_ADDR_LOW,
			      lower_16_bits(pbus_address));
	if (ret < 0)
		return ret;

	pbus_data_high = __mdiobus_read(bus, addr, AIR_BPBUS_RD_DATA_HIGH);
	if (pbus_data_high < 0)
		return pbus_data_high;

	pbus_data_low = __mdiobus_read(bus, addr, AIR_BPBUS_RD_DATA_LOW);
	if (pbus_data_low < 0)
		return pbus_data_low;

	pbus_data_old = pbus_data_low | (pbus_data_high << 16);
	pbus_data_new = (pbus_data_old & ~mask) | set;
	if (pbus_data_new == pbus_data_old)
		return 0;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_WR_ADDR_HIGH,
			      upper_16_bits(pbus_address));
	if (ret < 0)
		return ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_WR_ADDR_LOW,
			      lower_16_bits(pbus_address));
	if (ret < 0)
		return ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_WR_DATA_HIGH,
			      upper_16_bits(pbus_data_new));
	if (ret < 0)
		return ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_WR_DATA_LOW,
			      lower_16_bits(pbus_data_new));
	if (ret < 0)
		return ret;

	return 0;
}

int air_phy_buckpbus_reg_read(struct phy_device *phydev, u32 pbus_address,
			      u32 *pbus_data)
{
	int saved_page;
	int ret = 0;

	saved_page = phy_select_page(phydev, AIR_PHY_PAGE_EXTENDED_4);

	if (saved_page >= 0) {
		ret = __air_buckpbus_reg_read(phydev->mdio.bus,
					      phydev->mdio.addr,
					      pbus_address, pbus_data);
		if (ret < 0)
			phydev_err(phydev, "%s 0x%08x failed: %d\n", __func__,
				   pbus_address, ret);
	}

	return phy_restore_page(phydev, saved_page, ret);
}
EXPORT_SYMBOL_GPL(air_phy_buckpbus_reg_read);

int air_phy_buckpbus_reg_write(struct phy_device *phydev, u32 pbus_address,
			       u32 pbus_data)
{
	int saved_page;
	int ret = 0;

	saved_page = phy_select_page(phydev, AIR_PHY_PAGE_EXTENDED_4);

	if (saved_page >= 0) {
		ret = __air_buckpbus_reg_write(phydev->mdio.bus,
					       phydev->mdio.addr,
					       pbus_address, pbus_data);
		if (ret < 0)
			phydev_err(phydev, "%s 0x%08x failed: %d\n", __func__,
				   pbus_address, ret);
	}

	return phy_restore_page(phydev, saved_page, ret);
}
EXPORT_SYMBOL_GPL(air_phy_buckpbus_reg_write);

int air_phy_buckpbus_reg_modify(struct phy_device *phydev, u32 pbus_address,
				u32 mask, u32 set)
{
	int saved_page;
	int ret = 0;

	saved_page = phy_select_page(phydev, AIR_PHY_PAGE_EXTENDED_4);

	if (saved_page >= 0) {
		ret = __air_buckpbus_reg_modify(phydev->mdio.bus,
						phydev->mdio.addr,
						pbus_address, mask, set);
		if (ret < 0)
			phydev_err(phydev, "%s 0x%08x failed: %d\n", __func__,
				   pbus_address, ret);
	}

	return phy_restore_page(phydev, saved_page, ret);
}
EXPORT_SYMBOL_GPL(air_phy_buckpbus_reg_modify);

static int __air_write_buf(struct mii_bus *bus, int addr, u32 address,
			   const struct firmware *fw)
{
	unsigned int offset;
	int ret;
	u16 val;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_MODE,
			      AIR_BPBUS_MODE_ADDR_INCR);
	if (ret < 0)
		return ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_WR_ADDR_HIGH,
			      upper_16_bits(address));
	if (ret < 0)
		return ret;

	ret = __mdiobus_write(bus, addr, AIR_BPBUS_WR_ADDR_LOW,
			      lower_16_bits(address));
	if (ret < 0)
		return ret;

	for (offset = 0; offset < fw->size; offset += 4) {
		val = get_unaligned_le16(&fw->data[offset + 2]);
		ret = __mdiobus_write(bus, addr, AIR_BPBUS_WR_DATA_HIGH, val);
		if (ret < 0)
			return ret;

		val = get_unaligned_le16(&fw->data[offset]);
		ret = __mdiobus_write(bus, addr, AIR_BPBUS_WR_DATA_LOW, val);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/* The phy_select_page() path is not usable here: these run before any
 * phy_device exists. Callers hold the bus lock across select/op/restore.
 */
static int air_mdio_select_page(struct mii_bus *bus, int addr, int page)
{
	int saved_page, ret;

	saved_page = __mdiobus_read(bus, addr, AIR_EXT_PAGE_ACCESS);
	if (saved_page < 0)
		return saved_page;

	if (saved_page != page) {
		ret = __mdiobus_write(bus, addr, AIR_EXT_PAGE_ACCESS, page);
		if (ret < 0)
			return ret;
	}

	return saved_page;
}

static int air_mdio_restore_page(struct mii_bus *bus, int addr,
				 int saved_page, int page, int ret)
{
	int restore;

	if (saved_page != page) {
		restore = __mdiobus_write(bus, addr, AIR_EXT_PAGE_ACCESS,
					  saved_page);
		if (ret >= 0 && restore < 0)
			ret = restore;
	}

	return ret;
}

int air_fw_write_buf(struct mii_bus *bus, int addr, u32 address,
		     const struct firmware *fw)
{
	int saved_page, ret;

	mutex_lock(&bus->mdio_lock);

	saved_page = air_mdio_select_page(bus, addr, AIR_PHY_PAGE_EXTENDED_4);
	if (saved_page < 0) {
		ret = saved_page;
	} else {
		ret = __air_write_buf(bus, addr, address, fw);
		ret = air_mdio_restore_page(bus, addr, saved_page,
					    AIR_PHY_PAGE_EXTENDED_4, ret);
	}

	mutex_unlock(&bus->mdio_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(air_fw_write_buf);

static int air_mdio_buckpbus_reg_read(struct mii_bus *bus, int addr,
				      u32 pbus_address, u32 *pbus_data)
{
	int saved_page, ret;

	mutex_lock(&bus->mdio_lock);

	saved_page = air_mdio_select_page(bus, addr, AIR_PHY_PAGE_EXTENDED_4);
	if (saved_page < 0) {
		ret = saved_page;
	} else {
		ret = __air_buckpbus_reg_read(bus, addr, pbus_address,
					      pbus_data);
		ret = air_mdio_restore_page(bus, addr, saved_page,
					    AIR_PHY_PAGE_EXTENDED_4, ret);
	}

	mutex_unlock(&bus->mdio_lock);
	return ret;
}

static int air_mdio_buckpbus_reg_write(struct mii_bus *bus, int addr,
				       u32 pbus_address, u32 pbus_data)
{
	int saved_page, ret;

	mutex_lock(&bus->mdio_lock);

	saved_page = air_mdio_select_page(bus, addr, AIR_PHY_PAGE_EXTENDED_4);
	if (saved_page < 0) {
		ret = saved_page;
	} else {
		ret = __air_buckpbus_reg_write(bus, addr, pbus_address,
					       pbus_data);
		ret = air_mdio_restore_page(bus, addr, saved_page,
					    AIR_PHY_PAGE_EXTENDED_4, ret);
	}

	mutex_unlock(&bus->mdio_lock);
	return ret;
}

static int air_mdio_buckpbus_reg_modify(struct mii_bus *bus, int addr,
					u32 pbus_address, u32 mask, u32 set)
{
	int saved_page, ret;

	mutex_lock(&bus->mdio_lock);

	saved_page = air_mdio_select_page(bus, addr, AIR_PHY_PAGE_EXTENDED_4);
	if (saved_page < 0) {
		ret = saved_page;
	} else {
		ret = __air_buckpbus_reg_modify(bus, addr, pbus_address,
						mask, set);
		ret = air_mdio_restore_page(bus, addr, saved_page,
					    AIR_PHY_PAGE_EXTENDED_4, ret);
	}

	mutex_unlock(&bus->mdio_lock);
	return ret;
}

static int air_mmd_status_read(struct mii_bus *bus, int addr, bool is_c45)
{
	int ret;

	mutex_lock(&bus->mdio_lock);
	ret = mmd_phy_read(bus, addr, is_c45, MDIO_MMD_VEND1,
			   EN8811H_PHY_FW_STATUS);
	mutex_unlock(&bus->mdio_lock);

	return ret;
}

bool air_en8811h_mcu_running(struct mii_bus *bus, int addr, bool is_c45)
{
	return air_mmd_status_read(bus, addr, is_c45) == EN8811H_PHY_READY;
}
EXPORT_SYMBOL_GPL(air_en8811h_mcu_running);

int air_en8811h_wait_mcu_ready(struct mii_bus *bus, int addr, bool is_c45,
			       struct device *dev)
{
	int ret, reg_value;

	ret = air_mdio_buckpbus_reg_write(bus, addr, EN8811H_FW_CTRL_1,
					  EN8811H_FW_CTRL_1_FINISH);
	if (ret)
		return ret;

	/* Because of mdio-lock, may have to wait for multiple loads. A read
	 * error ends the poll at once, like phy_read_mmd_poll_timeout()
	 * would: the bus is not going to heal within the timeout.
	 */
	ret = read_poll_timeout(air_mmd_status_read, reg_value,
				reg_value < 0 ||
				reg_value == EN8811H_PHY_READY,
				20000, 7500000, true, bus, addr, is_c45);
	if (reg_value < 0)
		return reg_value;
	if (ret) {
		dev_err(dev, "MCU not ready: 0x%x\n", reg_value);
		return -ENODEV;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(air_en8811h_wait_mcu_ready);

int air_en8811h_fw_download(struct mii_bus *bus, int addr, bool is_c45,
			    struct device *dev, u32 *fw_version)
{
	const struct firmware *fw1, *fw2;
	int ret;

	if (air_en8811h_mcu_running(bus, addr, is_c45)) {
		/* Loaded by a bootloader, an earlier bind, or another
		 * device serving the chip. The wait below is what makes
		 * trusting the status register safe: a chip that was not
		 * in fact running fails there instead of coming up
		 * misprogrammed.
		 */
		ret = air_en8811h_wait_mcu_ready(bus, addr, is_c45, dev);
		if (ret < 0)
			return ret;

		ret = air_mdio_buckpbus_reg_read(bus, addr,
						 EN8811H_FW_VERSION,
						 fw_version);
		if (ret < 0)
			return ret;

		dev_info(dev, "MD32 already running, firmware %08x\n",
			 *fw_version);
		return 0;
	}

	ret = request_firmware_direct(&fw1, EN8811H_MD32_DM, dev);
	if (ret < 0)
		return ret;

	ret = request_firmware_direct(&fw2, EN8811H_MD32_DSP, dev);
	if (ret < 0)
		goto air_fw_download_rel1;

	ret = air_mdio_buckpbus_reg_write(bus, addr, EN8811H_FW_CTRL_1,
					  EN8811H_FW_CTRL_1_START);
	if (ret < 0)
		goto air_fw_download_out;

	ret = air_mdio_buckpbus_reg_modify(bus, addr, EN8811H_FW_CTRL_2,
					   EN8811H_FW_CTRL_2_LOADING,
					   EN8811H_FW_CTRL_2_LOADING);
	if (ret < 0)
		goto air_fw_download_out;

	ret = air_fw_write_buf(bus, addr, AIR_FW_ADDR_DM, fw1);
	if (ret < 0)
		goto air_fw_download_out;

	ret = air_fw_write_buf(bus, addr, AIR_FW_ADDR_DSP, fw2);
	if (ret < 0)
		goto air_fw_download_out;

	ret = air_mdio_buckpbus_reg_modify(bus, addr, EN8811H_FW_CTRL_2,
					   EN8811H_FW_CTRL_2_LOADING, 0);
	if (ret < 0)
		goto air_fw_download_out;

	ret = air_en8811h_wait_mcu_ready(bus, addr, is_c45, dev);
	if (ret < 0)
		goto air_fw_download_out;

	ret = air_mdio_buckpbus_reg_read(bus, addr, EN8811H_FW_VERSION,
					 fw_version);
	if (ret < 0)
		goto air_fw_download_out;

	dev_info(dev, "MD32 firmware version: %08x\n", *fw_version);

air_fw_download_out:
	release_firmware(fw2);

air_fw_download_rel1:
	release_firmware(fw1);

	/* No error print here: the callers retry or log on their own terms,
	 * and a poller retrying a half-installed firmware package would turn
	 * a print at this level into a permanent drumbeat.
	 */
	return ret;
}
EXPORT_SYMBOL_GPL(air_en8811h_fw_download);

MODULE_FIRMWARE(EN8811H_MD32_DM);
MODULE_FIRMWARE(EN8811H_MD32_DSP);

int air_phy_read_page(struct phy_device *phydev)
{
	return __phy_read(phydev, AIR_EXT_PAGE_ACCESS);
}
EXPORT_SYMBOL_GPL(air_phy_read_page);

int air_phy_write_page(struct phy_device *phydev, int page)
{
	return __phy_write(phydev, AIR_EXT_PAGE_ACCESS, page);
}
EXPORT_SYMBOL_GPL(air_phy_write_page);

MODULE_DESCRIPTION("Airoha PHY Library");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Louis-Alexis Eyraud");
