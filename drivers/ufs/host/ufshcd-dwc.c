// SPDX-License-Identifier: GPL-2.0-only
/*
 * UFS Host driver for Synopsys Designware Core
 *
 * Copyright (C) 2015-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * Authors: Joao Pinto <jpinto@synopsys.com>
 */

#include <linux/module.h>

#include <ufs/ufshcd.h>
#include <ufs/unipro.h>

#include "ufshcd-dwc.h"
#include "ufshci-dwc.h"

int ufshcd_dwc_dme_set_attrs(struct ufs_hba *hba,
				const struct ufshcd_dme_attr_val *v, int n)
{
	int ret = 0;
	int attr_node = 0;

	for (attr_node = 0; attr_node < n; attr_node++) {
		ret = ufshcd_dme_set_attr(hba, v[attr_node].attr_sel,
			ATTR_SET_NOR, v[attr_node].mib_val, v[attr_node].peer);

		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(ufshcd_dwc_dme_set_attrs);

/**
 * ufshcd_dwc_program_clk_div() - program clock divider.
 * @hba: Private Structure pointer
 * @divider_val: clock divider value to be programmed
 *
 */
static void ufshcd_dwc_program_clk_div(struct ufs_hba *hba, u32 divider_val)
{
	ufshcd_writel(hba, divider_val, DWC_UFS_REG_HCLKDIV);
}

/**
 * ufshcd_dwc_link_is_up() - check if link is up.
 * @hba: private structure pointer
 *
 * Return: 0 on success, non-zero value on failure.
 */
static int ufshcd_dwc_link_is_up(struct ufs_hba *hba)
{
	int dme_result = 0;

	ufshcd_dme_get(hba, UIC_ARG_MIB(VS_POWERSTATE), &dme_result);

	if (dme_result == UFSHCD_LINK_IS_UP) {
		ufshcd_set_link_active(hba);
		return 0;
	}

	return 1;
}

/**
 * ufshcd_dwc_connection_setup() - configure unipro attributes.
 * @hba: pointer to drivers private data
 *
 * This function configures both the local side (host) and the peer side
 * (device) unipro attributes to establish the connection to application/
 * cport.
 * This function is not required if the hardware is properly configured to
 * have this connection setup on reset. But invoking this function does no
 * harm and should be fine even working with any ufs device.
 *
 * Return: 0 on success non-zero value on failure.
 */
static int ufshcd_dwc_connection_setup(struct ufs_hba *hba)
{
	static const struct ufshcd_dme_attr_val setup_attrs[] = {
		{ UIC_ARG_MIB(T_CONNECTIONSTATE), 0, DME_LOCAL },
		{ UIC_ARG_MIB(N_DEVICEID), 0, DME_LOCAL },
		{ UIC_ARG_MIB(N_DEVICEID_VALID), 0, DME_LOCAL },
		{ UIC_ARG_MIB(T_PEERDEVICEID), 1, DME_LOCAL },
		{ UIC_ARG_MIB(T_PEERCPORTID), 0, DME_LOCAL },
		{ UIC_ARG_MIB(T_TRAFFICCLASS), 0, DME_LOCAL },
		{ UIC_ARG_MIB(T_CPORTFLAGS), 0x6, DME_LOCAL },
		{ UIC_ARG_MIB(T_CPORTMODE), 1, DME_LOCAL },
		{ UIC_ARG_MIB(T_CONNECTIONSTATE), 1, DME_LOCAL },
		{ UIC_ARG_MIB(T_CONNECTIONSTATE), 0, DME_PEER },
		{ UIC_ARG_MIB(N_DEVICEID), 1, DME_PEER },
		{ UIC_ARG_MIB(N_DEVICEID_VALID), 1, DME_PEER },
		{ UIC_ARG_MIB(T_PEERDEVICEID), 1, DME_PEER },
		{ UIC_ARG_MIB(T_PEERCPORTID), 0, DME_PEER },
		{ UIC_ARG_MIB(T_TRAFFICCLASS), 0, DME_PEER },
		{ UIC_ARG_MIB(T_CPORTFLAGS), 0x6, DME_PEER },
		{ UIC_ARG_MIB(T_CPORTMODE), 1, DME_PEER },
		{ UIC_ARG_MIB(T_CONNECTIONSTATE), 1, DME_PEER }
	};

	return ufshcd_dwc_dme_set_attrs(hba, setup_attrs, ARRAY_SIZE(setup_attrs));
}

/**
 * ufshcd_dwc_link_startup_notify() - program clock divider.
 * @hba: private structure pointer
 * @status: Callback notify status
 *
 * Return: 0 on success, non-zero value on failure.
 */
int ufshcd_dwc_link_startup_notify(struct ufs_hba *hba,
					enum ufs_notify_change_status status)
{
	int err = 0;

	if (status == PRE_CHANGE) {
		ufshcd_dwc_program_clk_div(hba, DWC_UFS_REG_HCLKDIV_DIV_125);

		err = ufshcd_vops_phy_initialization(hba);
		if (err) {
			dev_err(hba->dev, "Phy setup failed (%d)\n", err);
			goto out;
		}
	} else { /* POST_CHANGE */
		err = ufshcd_dwc_link_is_up(hba);
		if (err) {
			dev_err(hba->dev, "Link is not up\n");
			goto out;
		}

		err = ufshcd_dwc_connection_setup(hba);
		if (err)
			dev_err(hba->dev, "Connection setup failed (%d)\n",
									err);
	}

out:
	return err;
}
EXPORT_SYMBOL(ufshcd_dwc_link_startup_notify);

/**
 * ufshcd_dwc_phy_reg_write - Write a DWC M-PHY CREG register
 * @hba: private structure pointer
 * @addr: M-PHY CREG register address
 * @val: value to write
 *
 * Write a 16-bit M-PHY CREG register through the Synopsys DesignWare
 * UniPro indirect register access interface.
 *
 * Return: 0 on success, non-zero value on failure.
 */
int ufshcd_dwc_phy_reg_write(struct ufs_hba *hba, u32 addr, u32 val)
{
	struct ufshcd_dme_attr_val phy_write_attrs[] = {
		{ UIC_ARG_MIB(CBCREGADDRLSB), 0, DME_LOCAL },
		{ UIC_ARG_MIB(CBCREGADDRMSB), 0, DME_LOCAL },
		{ UIC_ARG_MIB(CBCREGWRLSB), 0, DME_LOCAL },
		{ UIC_ARG_MIB(CBCREGWRMSB), 0, DME_LOCAL },
		{ UIC_ARG_MIB(CBCREGRDWRSEL), 1, DME_LOCAL },
		{ UIC_ARG_MIB(VS_MPHYCFGUPDT), 1, DME_LOCAL }
	};

	phy_write_attrs[0].mib_val = (u8)addr;
	phy_write_attrs[1].mib_val = (u8)(addr >> 8);
	phy_write_attrs[2].mib_val = (u8)val;
	phy_write_attrs[3].mib_val = (u8)(val >> 8);

	return ufshcd_dwc_dme_set_attrs(hba, phy_write_attrs,
					ARRAY_SIZE(phy_write_attrs));
}
EXPORT_SYMBOL(ufshcd_dwc_phy_reg_write);

/**
 * ufshcd_dwc_phy_reg_read - Read a DWC M-PHY CREG register
 * @hba: private structure pointer
 * @addr: M-PHY CREG register address
 * @val: pointer where the read value is stored
 *
 * Read a 16-bit M-PHY CREG register through the Synopsys DesignWare
 * UniPro indirect register access interface.
 *
 * Return: 0 on success, non-zero value on failure.
 */
int ufshcd_dwc_phy_reg_read(struct ufs_hba *hba, u32 addr, u32 *val)
{
	struct ufshcd_dme_attr_val phy_read_attrs[] = {
		{ UIC_ARG_MIB(CBCREGADDRLSB), 0, DME_LOCAL },
		{ UIC_ARG_MIB(CBCREGADDRMSB), 0, DME_LOCAL },
		{ UIC_ARG_MIB(CBCREGRDWRSEL), 0, DME_LOCAL },
		{ UIC_ARG_MIB(VS_MPHYCFGUPDT), 1, DME_LOCAL }
	};
	u32 mib_val;
	int ret;

	phy_read_attrs[0].mib_val = (u8)addr;
	phy_read_attrs[1].mib_val = (u8)(addr >> 8);

	ret = ufshcd_dwc_dme_set_attrs(hba, phy_read_attrs,
				       ARRAY_SIZE(phy_read_attrs));
	if (ret)
		return ret;

	ret = ufshcd_dme_get(hba, UIC_ARG_MIB(CBCREGRDLSB), &mib_val);
	if (ret)
		return ret;

	*val = mib_val;
	ret = ufshcd_dme_get(hba, UIC_ARG_MIB(CBCREGRDMSB), &mib_val);
	if (ret)
		return ret;

	*val |= (mib_val << 8);

	return 0;
}
EXPORT_SYMBOL(ufshcd_dwc_phy_reg_read);

MODULE_AUTHOR("Joao Pinto <Joao.Pinto@synopsys.com>");
MODULE_DESCRIPTION("UFS Host driver for Synopsys Designware Core");
MODULE_LICENSE("Dual BSD/GPL");
