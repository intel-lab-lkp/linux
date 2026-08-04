// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD eSPI Controller Driver
 * Copyright (c) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/espi/espi.h>

#include "espi-amd.h"

static void amd_espi_clear_status(struct amd_espi_data *priv)
{
	u32 status = readl(priv->base + AMD_ESPI_SLAVE0_INT_STS_REG);

	if (status)
		writel(status, priv->base + AMD_ESPI_SLAVE0_INT_STS_REG);
}

static int amd_espi_check_status(struct amd_espi_data *priv, u32 status)
{
	if (!(status & AMD_ESPI_DNCMD_INT)) {
		dev_err(priv->dev, "downstream command did not complete\n");
		return -EIO;
	}
	if (!(status & AMD_ESPI_ERR_INT_MASK))
		return 0;

	if (status & AMD_ESPI_WAIT_TIMEOUT_INT) {
		dev_err(priv->dev, "wait-state timer timeout\n");
		return -ETIMEDOUT;
	}
	if (status & AMD_ESPI_NO_RESP_INT) {
		dev_err(priv->dev, "no response from target\n");
		return -ENODEV;
	}
	if (status & AMD_ESPI_CRC_ERR_INT) {
		dev_err(priv->dev, "CRC error\n");
		return -EBADMSG;
	}
	dev_err(priv->dev, "command error, status=0x%08x\n", status);
	return -EIO;
}

static int amd_espi_send_cmd(struct amd_espi_data *priv,
			     u32 hdr0, u32 hdr1, u32 hdr2)
{
	u32 status;
	int ret;

	ret = readl_poll_timeout(priv->base + AMD_ESPI_DN_TXHDR_REG0, status,
				 !(status & AMD_ESPI_TXHDR0_CMD_STATUS),
				 AMD_ESPI_MSG_DELAY_MIN_US,
				 AMD_ESPI_RESP_MAX_TIMEOUT_US);
	if (ret) {
		dev_err(priv->dev, "controller not ready to accept command\n");
		return -EBUSY;
	}

	amd_espi_clear_status(priv);

	writel(hdr1, priv->base + AMD_ESPI_DN_TXHDR_REG1);
	writel(hdr2, priv->base + AMD_ESPI_DN_TXHDR_REG2);
	/*
	 * Data port must be written before triggering to frame the packet.
	 * Channel-independent commands carry no data payload, so zero suffices.
	 */
	writel(0, priv->base + AMD_ESPI_DN_TXDATA_REG0);

	dev_dbg(priv->dev,
		"TX before trigger: hdr0=0x%08x hdr1=0x%08x hdr2=0x%08x data=0x%08x\n",
		hdr0, hdr1, hdr2, 0);

	writel(hdr0, priv->base + AMD_ESPI_DN_TXHDR_REG0);

	ret = readl_poll_timeout(priv->base + AMD_ESPI_DN_TXHDR_REG0, status,
				 !(status & AMD_ESPI_TXHDR0_CMD_STATUS),
				 AMD_ESPI_MSG_DELAY_MIN_US,
				 AMD_ESPI_RESP_MAX_TIMEOUT_US);
	if (ret) {
		dev_err(priv->dev, "timed out sending command\n");
		return -ETIMEDOUT;
	}

	ret = readl_poll_timeout(priv->base + AMD_ESPI_SLAVE0_INT_STS_REG,
				 status,
				 status & (AMD_ESPI_DNCMD_INT | AMD_ESPI_ERR_INT_MASK),
				 AMD_ESPI_MSG_DELAY_MIN_US,
				 AMD_ESPI_RESP_MAX_TIMEOUT_US);
	if (ret) {
		dev_err(priv->dev, "timed out waiting for completion\n");
		return -ETIMEDOUT;
	}

	ret = amd_espi_check_status(priv, status);
	writel(AMD_ESPI_DNCMD_INT, priv->base + AMD_ESPI_SLAVE0_INT_STS_REG);
	return ret;
}

static u32 amd_espi_cfg_hdr0(enum espi_cmd_type type, u32 addr)
{
	return FIELD_PREP(AMD_ESPI_TXHDR0_CMD_TYPE, type) |
	       AMD_ESPI_TXHDR0_CMD_STATUS |
	       FIELD_PREP(AMD_ESPI_TXHDR0_HDATA0, (addr >> 8) & 0xff) |
	       FIELD_PREP(AMD_ESPI_TXHDR0_HDATA1, addr & 0xff);
}

static int amd_espi_get_configuration(struct espi_controller *ctrl,
				      u32 slave_reg_addr, u32 *config)
{
	struct amd_espi_data *priv = espi_controller_get_devdata(ctrl);
	u32 hdr0 = amd_espi_cfg_hdr0(ESPI_CMD_GET_CONFIGURATION, slave_reg_addr);
	int ret;

	ret = amd_espi_send_cmd(priv, hdr0, 0, 0);
	if (ret)
		return ret;

	*config = readl(priv->base + AMD_ESPI_DN_TXHDR_REG1);
	dev_dbg(priv->dev, "GET_CONFIGURATION addr=0x%04x config=0x%08x\n",
		slave_reg_addr, *config);
	return 0;
}

/* Translate: target and host use different bit positions and clock encoding. */
static void amd_espi_sync_config(struct amd_espi_data *priv, u32 slave_cfg)
{
	u32 io_mode = FIELD_GET(ESPI_GENCFG_IO_MODE, slave_cfg);
	u32 host_freq;
	u32 reg;

	switch (FIELD_GET(ESPI_GENCFG_OP_FREQ, slave_cfg)) {
	case ESPI_GENCFG_FREQ_66MHZ:
		host_freq = AMD_ESPI_SLAVE0_CLK_FREQ_66MHZ;
		break;
	case ESPI_GENCFG_FREQ_33MHZ:
		host_freq = AMD_ESPI_SLAVE0_CLK_FREQ_33MHZ;
		break;
	default:
		host_freq = AMD_ESPI_SLAVE0_CLK_FREQ_16MHZ;
		break;
	}

	reg = readl(priv->base + AMD_ESPI_SLAVE0_CONFIG_REG);
	reg &= ~(AMD_ESPI_SLAVE0_CFG_CLK_FREQ | AMD_ESPI_SLAVE0_CFG_IO_MODE |
		 AMD_ESPI_SLAVE0_CFG_ALERT_MODE | AMD_ESPI_SLAVE0_CFG_CRC_EN);
	reg |= FIELD_PREP(AMD_ESPI_SLAVE0_CFG_CLK_FREQ, host_freq);
	reg |= FIELD_PREP(AMD_ESPI_SLAVE0_CFG_IO_MODE, io_mode);
	if (slave_cfg & ESPI_GENCFG_ALERT_MODE)
		reg |= AMD_ESPI_SLAVE0_CFG_ALERT_MODE;
	if (slave_cfg & ESPI_GENCFG_CRC_EN)
		reg |= AMD_ESPI_SLAVE0_CFG_CRC_EN;
	writel(reg, priv->base + AMD_ESPI_SLAVE0_CONFIG_REG);

	dev_dbg(priv->dev,
		"host SLAVE0_CONFIG updated to 0x%08x (io=%u host_freq=%u)\n",
		readl(priv->base + AMD_ESPI_SLAVE0_CONFIG_REG), io_mode, host_freq);
}

static int amd_espi_set_configuration(struct espi_controller *ctrl,
				      u32 slave_reg_addr, u32 config)
{
	struct amd_espi_data *priv = espi_controller_get_devdata(ctrl);
	u32 hdr0 = amd_espi_cfg_hdr0(ESPI_CMD_SET_CONFIGURATION, slave_reg_addr);
	int ret;

	dev_dbg(priv->dev, "SET_CONFIGURATION addr=0x%04x config=0x%08x\n",
		slave_reg_addr, config);
	ret = amd_espi_send_cmd(priv, hdr0, config, 0);
	if (ret)
		return ret;

	/* Sync host link parameters after a General Configuration change. */
	if (slave_reg_addr == ESPI_SLAVE_REG_GENERAL_CFG)
		amd_espi_sync_config(priv, config);

	return 0;
}

/* Lower clock to 16.7 MHz for In-Band Reset; preserve I/O mode. */
static void amd_espi_lower_clk(struct amd_espi_data *priv)
{
	u32 reg = readl(priv->base + AMD_ESPI_SLAVE0_CONFIG_REG);

	reg &= ~AMD_ESPI_SLAVE0_CFG_CLK_FREQ;
	reg |= FIELD_PREP(AMD_ESPI_SLAVE0_CFG_CLK_FREQ,
			  AMD_ESPI_SLAVE0_CLK_FREQ_16MHZ);
	writel(reg, priv->base + AMD_ESPI_SLAVE0_CONFIG_REG);
}

static const char *amd_espi_io_mode_str(u32 io_mode)
{
	switch (io_mode) {
	case AMD_ESPI_SLAVE0_IO_MODE_SINGLE:
		return "single";
	case AMD_ESPI_SLAVE0_IO_MODE_DUAL:
		return "dual";
	case AMD_ESPI_SLAVE0_IO_MODE_QUAD:
		return "quad";
	default:
		return "reserved";
	}
}

static const char *amd_espi_clk_freq_str(u32 clk_freq)
{
	switch (clk_freq) {
	case AMD_ESPI_SLAVE0_CLK_FREQ_16MHZ:
		return "16 MHz";
	case AMD_ESPI_SLAVE0_CLK_FREQ_33MHZ:
		return "33 MHz";
	case AMD_ESPI_SLAVE0_CLK_FREQ_66MHZ:
		return "66 MHz";
	default:
		return "reserved";
	}
}

static void amd_espi_log_link(struct amd_espi_data *priv, const char *when)
{
	u32 reg = readl(priv->base + AMD_ESPI_SLAVE0_CONFIG_REG);

	dev_dbg(priv->dev,
		"%s: I/O mode=%s frequency=%s (SLAVE0_CONFIG=0x%08x)\n", when,
		amd_espi_io_mode_str(FIELD_GET(AMD_ESPI_SLAVE0_CFG_IO_MODE, reg)),
		amd_espi_clk_freq_str(FIELD_GET(AMD_ESPI_SLAVE0_CFG_CLK_FREQ, reg)),
		reg);
}

static int amd_espi_inband_reset(struct espi_controller *ctrl)
{
	struct amd_espi_data *priv = espi_controller_get_devdata(ctrl);
	u32 hdr0 = FIELD_PREP(AMD_ESPI_TXHDR0_CMD_TYPE, ESPI_CMD_IN_BAND_RESET) |
		   AMD_ESPI_TXHDR0_CMD_STATUS;
	u32 cfg_before;
	u32 reg;
	int ret;

	cfg_before = readl(priv->base + AMD_ESPI_SLAVE0_CONFIG_REG);
	dev_dbg(priv->dev, "IN_BAND_RESET: SLAVE0_CONFIG before = 0x%08x\n",
		cfg_before);

	amd_espi_lower_clk(priv);

	ret = amd_espi_send_cmd(priv, hdr0, 0, 0);
	if (ret)
		return ret;

	/*
	 * Restore CRC and Alert-mode bits after reset: the controller clears
	 * SLAVE0_CONFIG but the target retains these across an In-Band Reset.
	 */
	reg = readl(priv->base + AMD_ESPI_SLAVE0_CONFIG_REG);
	if (ctrl->caps.alert_mode)
		reg |= AMD_ESPI_SLAVE0_CFG_ALERT_MODE;
	if (ctrl->caps.crc_supported)
		reg |= AMD_ESPI_SLAVE0_CFG_CRC_EN;
	writel(reg, priv->base + AMD_ESPI_SLAVE0_CONFIG_REG);

	dev_dbg(priv->dev,
		"IN_BAND_RESET: SLAVE0_CONFIG after = 0x%08x (was 0x%08x)\n",
		readl(priv->base + AMD_ESPI_SLAVE0_CONFIG_REG), cfg_before);

	amd_espi_log_link(priv, "after in-band reset");
	return 0;
}

static int amd_espi_setup(struct espi_controller *ctrl)
{
	struct amd_espi_data *priv = espi_controller_get_devdata(ctrl);
	u32 reg;

	amd_espi_log_link(priv, "firmware default");

	writel(AMD_ESPI_INTR_MASK, priv->base + AMD_ESPI_SLAVE0_INT_STS_REG);

	/* Controller-wide bring-up: must run before the first downstream command. */
	reg = readl(priv->base + AMD_ESPI_SLAVE0_RX_VW_REG);
	writel(reg | AMD_ESPI_SLAVE0_RX_VW_INIT_MASK,
	       priv->base + AMD_ESPI_SLAVE0_RX_VW_REG);

	reg = readl(priv->base + AMD_ESPI_GLOBAL_CNTRL_REG0);
	reg |= AMD_ESPI_GLOBAL0_WDG_EN | AMD_ESPI_GLOBAL0_WAIT_CHK_EN;
	reg &= ~AMD_ESPI_GLOBAL0_WAIT_STATE;
	reg |= FIELD_PREP(AMD_ESPI_GLOBAL0_WAIT_STATE, AMD_ESPI_WAIT_STATE_CNT);
	writel(reg, priv->base + AMD_ESPI_GLOBAL_CNTRL_REG0);

	reg = readl(priv->base + AMD_ESPI_SLAVE0_INT_EN_REG);
	writel(reg | AMD_ESPI_ALL_ERR_INT | AMD_ESPI_REG_CMD_INT,
	       priv->base + AMD_ESPI_SLAVE0_INT_EN_REG);

	reg = readl(priv->base + AMD_ESPI_GLOBAL_CNTRL_REG1);
	reg &= ~(AMD_ESPI_ERR_INT_MAP | AMD_ESPI_RGCMD_INT_MAP);
	reg |= FIELD_PREP(AMD_ESPI_ERR_INT_MAP, AMD_ESPI_INT_MAP_SMI);
	reg |= FIELD_PREP(AMD_ESPI_RGCMD_INT_MAP, AMD_ESPI_INT_MAP_SMI);
	reg |= AMD_ESPI_BUS_MASTER_EN | AMD_ESPI_VW_REQ_EN;
	writel(reg, priv->base + AMD_ESPI_GLOBAL_CNTRL_REG1);

	/*
	 * Unmask the VW IRQ index window. Do not touch SLAVE0_CONFIG clock or
	 * I/O mode: preserve the operating point firmware already negotiated.
	 */
	reg = readl(priv->base + AMD_ESPI_SLAVE0_VW_MISC_CNTRL_REG);
	reg &= ~GENMASK(31, 8);
	writel(reg | GENMASK(3, 0),
	       priv->base + AMD_ESPI_SLAVE0_VW_MISC_CNTRL_REG);

	writel(AMD_ESPI_INTR_MASK, priv->base + AMD_ESPI_SLAVE0_INT_STS_REG);

	amd_espi_log_link(priv, "after probe setup");
	return 0;
}

/* .get_status omitted: AMD HW has no GET_STATUS wire command. */
static const struct espi_controller_ops amd_espi_ops = {
	.setup			= amd_espi_setup,
	.get_configuration	= amd_espi_get_configuration,
	.set_configuration	= amd_espi_set_configuration,
	.inband_reset		= amd_espi_inband_reset,
};

static u32 amd_espi_read_caps(struct amd_espi_data *priv,
			      struct espi_capabilities *caps)
{
	u32 cap = readl(priv->base + AMD_ESPI_MASTER_CAP_REG);

	dev_dbg(priv->dev, "MASTER_CAP=0x%08x\n", cap);

	caps->supported_channels = 0;
	if (cap & AMD_ESPI_CAP_PR_SUPPORT)
		caps->supported_channels |= ESPI_CHANNEL_PERIPH_SUPP;
	if (cap & AMD_ESPI_CAP_VW_SUPPORT)
		caps->supported_channels |= ESPI_CHANNEL_VWIRE_SUPP;
	if (cap & AMD_ESPI_CAP_OOB_SUPPORT)
		caps->supported_channels |= ESPI_CHANNEL_OOB_SUPP;
	if (cap & AMD_ESPI_CAP_FLASH_SUPPORT)
		caps->supported_channels |= ESPI_CHANNEL_FLASH_SUPP;

	switch (FIELD_GET(AMD_ESPI_CAP_CLK_FREQ, cap)) {
	case 0x3:
		caps->max_freq_mhz = ESPI_FREQ_66MHZ;
		break;
	case 0x1:
		caps->max_freq_mhz = ESPI_FREQ_33MHZ;
		break;
	default:
		caps->max_freq_mhz = ESPI_FREQ_16MHZ;
		break;
	}
	switch (FIELD_GET(AMD_ESPI_CAP_IO_MODE, cap)) {
	case 0x2:
		caps->io_mode = ESPI_IO_MODE_QUAD;
		break;
	case 0x1:
		caps->io_mode = ESPI_IO_MODE_DUAL;
		break;
	default:
		caps->io_mode = ESPI_IO_MODE_SINGLE;
		break;
	}

	caps->alert_mode = !!(cap & AMD_ESPI_CAP_ALERT_MODE);
	caps->crc_supported = !!(cap & AMD_ESPI_CAP_CRC_CHECK);
	caps->periph_max_payload = FIELD_GET(AMD_ESPI_CAP_PR_MAX_SIZE, cap);
	caps->vwire_max_count = FIELD_GET(AMD_ESPI_CAP_VW_MAX_SIZE, cap);
	caps->oob_max_payload = FIELD_GET(AMD_ESPI_CAP_OOB_MAX_SIZE, cap);
	caps->flash_max_payload = FIELD_GET(AMD_ESPI_CAP_FLASH_MAX_SIZE, cap);

	return cap;
}

static u8 amd_espi_num_targets(u32 cap)
{
	/* CAP_SLAVE_NUM is 0-based: 0 means 1 target, 1 means 2, etc. */
	return (u8)(FIELD_GET(AMD_ESPI_CAP_SLAVE_NUM, cap) + 1);
}

static int amd_espi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct espi_controller *ctrl;
	struct amd_espi_data *priv;
	int ret;
	u32 cap;

	ctrl = espi_controller_alloc(dev, sizeof(*priv));
	if (IS_ERR(ctrl))
		return dev_err_probe(dev, PTR_ERR(ctrl),
				     "failed to allocate eSPI controller\n");

	priv = espi_controller_get_devdata(ctrl);
	priv->dev = dev;
	priv->version = (enum amd_espi_versions)(uintptr_t)
			device_get_match_data(dev);

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->base),
				    "failed to map registers\n");
		goto err_put;
	}

	cap = amd_espi_read_caps(priv, &ctrl->caps);
	ctrl->ops = &amd_espi_ops;
	ctrl->max_targets = amd_espi_num_targets(cap);

	ret = espi_controller_register(ctrl);
	if (ret) {
		ret = dev_err_probe(dev, ret,
				    "failed to register eSPI controller\n");
		goto err_put;
	}

	platform_set_drvdata(pdev, ctrl);
	return 0;

err_put:
	espi_controller_put(ctrl);
	return ret;
}

static void amd_espi_remove(struct platform_device *pdev)
{
	espi_controller_unregister(platform_get_drvdata(pdev));
}

static const struct acpi_device_id amd_espi_acpi_match[] = {
	{ "AMDI0070", AMD_ESPI_V1 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, amd_espi_acpi_match);

static struct platform_driver amd_espi_driver = {
	.driver = {
		.name			= "amd-espi",
		.acpi_match_table	= amd_espi_acpi_match,
	},
	.probe	= amd_espi_probe,
	.remove	= amd_espi_remove,
};
module_platform_driver(amd_espi_driver);

MODULE_AUTHOR("Krishnamoorthi M <krishnamoorthi.m@amd.com>");
MODULE_AUTHOR("Akshata MukundShetty <akshata.mukundshetty@amd.com>");
MODULE_DESCRIPTION("AMD eSPI Controller Driver");
MODULE_LICENSE("GPL");
