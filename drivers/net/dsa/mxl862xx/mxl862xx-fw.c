// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Firmware flash and devlink support for MaxLinear MxL862xx
 *
 * Copyright (C) 2025 Daniel Golle <daniel@makrotopia.org>
 */

#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/rtnetlink.h>
#include <net/dsa.h>

#include "mxl862xx.h"
#include "mxl862xx-api.h"
#include "mxl862xx-cmd.h"
#include "mxl862xx-fw.h"
#include "mxl862xx-host.h"

/* SB PDI registers (clause-22 SMDIO address space) */
#define MXL862XX_SB_PDI_CTRL		0xe100
#define MXL862XX_SB_PDI_ADDR		0xe101
#define MXL862XX_SB_PDI_DATA		0xe102
#define MXL862XX_SB_PDI_STAT		0xe103

/* SB PDI CTRL modes */
#define MXL862XX_SB_PDI_CTRL_RST	0x00
#define MXL862XX_SB_PDI_CTRL_WR	0x02

/* SB PDI handshake magic */
#define MXL862XX_SB_PDI_READY		0xc55c
#define MXL862XX_SB_PDI_START		0xf48f
#define MXL862XX_SB_PDI_END		0x3cc3

/* Firmware transfer geometry */
#define MXL862XX_FW_HDR_SIZE		20
#define MXL862XX_FW_BANK_HALF		16384	/* words per half-bank */
#define MXL862XX_FW_BANK_SLICE		32760	/* words per full slice */
#define MXL862XX_FW_SB1_ADDR		0x7800	/* SB1 word address */

/* Timeouts (generous upper bounds) */
#define MXL862XX_FW_READY_TIMEOUT_MS	30000
#define MXL862XX_FW_ACK_TIMEOUT_MS	5000
#define MXL862XX_FW_ERASE_TIMEOUT_MS	300000
#define MXL862XX_FW_WRITE_TIMEOUT_MS	120000
#define MXL862XX_FW_REBOOT_DELAY_MS	5000

static void mxl862xx_sb_pdi_reset(struct mxl862xx_priv *priv)
{
	mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_CTRL,
			     MXL862XX_SB_PDI_CTRL_RST);
	mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_ADDR,
			     MXL862XX_SB_PDI_CTRL_RST);
	mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_DATA,
			     MXL862XX_SB_PDI_CTRL_RST);
}

static int mxl862xx_sb_pdi_poll_stat(struct mxl862xx_priv *priv, u16 expected,
				     unsigned long timeout_ms)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);
	int ret;

	do {
		ret = mxl862xx_smdio_read(priv, MXL862XX_SB_PDI_STAT);
		if (ret < 0)
			return ret;
		if ((u16)ret == expected)
			return 0;
		usleep_range(10000, 11000);
	} while (time_before(jiffies, timeout));

	return -ETIMEDOUT;
}

static int mxl862xx_sb_pdi_flush_slice(struct mxl862xx_priv *priv,
				       u32 data_written)
{
	mxl862xx_sb_pdi_reset(priv);
	mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_STAT, data_written);

	return mxl862xx_sb_pdi_poll_stat(priv, 0,
					 MXL862XX_FW_WRITE_TIMEOUT_MS);
}

static void mxl862xx_flash_notify(struct devlink *dl, const char *status,
				  u32 done, u32 total)
{
	devlink_flash_update_status_notify(dl, status, NULL, done, total);
}

/**
 * mxl862xx_rescue_mode_detect - check whether the switch sits in MCUboot
 * @priv: driver private data
 *
 * MCUboot signals readiness for a firmware download with the SB PDI
 * ready magic; only the clause-22 SMDIO interface works in this state.
 *
 * Return: true if MCUboot is waiting for a firmware download.
 */
bool mxl862xx_rescue_mode_detect(struct mxl862xx_priv *priv)
{
	int ret;

	ret = mxl862xx_smdio_read(priv, MXL862XX_SB_PDI_STAT);

	return ret == MXL862XX_SB_PDI_READY;
}

/* device_reprobe() -> remove() frees priv while the work runs, so
 * the work struct cannot live in mxl862xx_priv.
 */
struct mxl862xx_reprobe {
	struct device *dev;
	struct delayed_work dwork;
};

static void mxl862xx_reprobe_work_fn(struct work_struct *work)
{
	struct mxl862xx_reprobe *reprobe =
		container_of(work, struct mxl862xx_reprobe, dwork.work);

	if (device_reprobe(reprobe->dev))
		dev_err(reprobe->dev, "reprobe failed\n");
	put_device(reprobe->dev);
	kfree(reprobe);
	module_put(THIS_MODULE);
}

/* MCUboot firmware image header */
struct mxl862xx_fw_hdr {
	__le32 image_type;
	__le32 image_size_1;
	__le32 image_checksum_1;
	__le32 image_size_2;
	__le32 image_checksum_2;
} __packed;

static int mxl862xx_flash_firmware(struct mxl862xx_priv *priv,
				   const struct firmware *fw,
				   struct devlink *dl)
{
	const struct mxl862xx_fw_hdr *hdr;
	u32 word_idx = 0, data_written = 0, idx = 0;
	unsigned long next_notify = 0;
	const u8 *payload;
	u32 payload_size;
	u16 word, fdata;
	int ret, i;
	u32 crc;

	if (fw->size < MXL862XX_FW_HDR_SIZE)
		return -EINVAL;

	hdr = (const struct mxl862xx_fw_hdr *)fw->data;
	payload = fw->data + MXL862XX_FW_HDR_SIZE;
	payload_size = le32_to_cpu(hdr->image_size_1) +
		       le32_to_cpu(hdr->image_size_2);

	if (payload_size > fw->size - MXL862XX_FW_HDR_SIZE) {
		dev_err(&priv->mdiodev->dev,
			"flash: firmware file too small for declared size\n");
		return -EINVAL;
	}

	if (le32_to_cpu(hdr->image_size_1)) {
		crc = ~crc32_le(~0U, payload,
				le32_to_cpu(hdr->image_size_1));
		if (crc != le32_to_cpu(hdr->image_checksum_1)) {
			dev_err(&priv->mdiodev->dev,
				"flash: image 1 CRC mismatch (got %08x, expected %08x)\n",
				crc, le32_to_cpu(hdr->image_checksum_1));
			return -EINVAL;
		}
	}

	if (le32_to_cpu(hdr->image_size_2)) {
		crc = ~crc32_le(~0U,
				payload + le32_to_cpu(hdr->image_size_1),
				le32_to_cpu(hdr->image_size_2));
		if (crc != le32_to_cpu(hdr->image_checksum_2)) {
			dev_err(&priv->mdiodev->dev,
				"flash: image 2 CRC mismatch (got %08x, expected %08x)\n",
				crc, le32_to_cpu(hdr->image_checksum_2));
			return -EINVAL;
		}
	}

	/* Step 1: reboot the firmware into MCUboot rescue mode */
	if (!priv->rescue_mode) {
		ret = mxl862xx_api_wrap(priv, SYS_MISC_FW_UPDATE, NULL, 0,
					false, false);
		if (ret) {
			dev_err(&priv->mdiodev->dev,
				"flash: FW_UPDATE command failed: %pe\n",
				ERR_PTR(ret));
			return ret;
		}
	}

	/* Failures from here on must go through end_magic so MCUboot
	 * reboots instead of waiting forever.
	 */

	/* Step 2: wait for bootloader ready */
	mxl862xx_flash_notify(dl, "Waiting for bootloader", 0, 0);
	mxl862xx_sb_pdi_reset(priv);
	ret = mxl862xx_sb_pdi_poll_stat(priv, MXL862XX_SB_PDI_READY,
					MXL862XX_FW_READY_TIMEOUT_MS);
	if (ret) {
		dev_err(&priv->mdiodev->dev,
			"flash: bootloader not ready: %pe\n", ERR_PTR(ret));
		goto end_magic;
	}

	/* Step 3: start handshake */
	mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_STAT,
			     MXL862XX_SB_PDI_START);
	ret = mxl862xx_sb_pdi_poll_stat(priv, MXL862XX_SB_PDI_START + 1,
					MXL862XX_FW_ACK_TIMEOUT_MS);
	if (ret) {
		dev_err(&priv->mdiodev->dev,
			"flash: start handshake failed: %pe\n", ERR_PTR(ret));
		goto end_magic;
	}

	/* Step 4: transfer image header */
	mxl862xx_flash_notify(dl, "Erasing flash", 0, 0);
	mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_CTRL,
			     MXL862XX_SB_PDI_CTRL_WR);
	for (i = 0; i < MXL862XX_FW_HDR_SIZE / 2; i++) {
		word = fw->data[i * 2] |
		       ((u16)fw->data[i * 2 + 1] << 8);
		mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_DATA, word);
	}
	mxl862xx_sb_pdi_reset(priv);

	/* the byte count in STAT triggers the erase */
	mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_STAT,
			     MXL862XX_FW_HDR_SIZE);

	/* ACK is byte count + 1 */
	ret = mxl862xx_sb_pdi_poll_stat(priv, MXL862XX_FW_HDR_SIZE + 1,
					MXL862XX_FW_ACK_TIMEOUT_MS);
	if (ret) {
		dev_err(&priv->mdiodev->dev,
			"flash: header ACK failed: %pe\n", ERR_PTR(ret));
		goto end_magic;
	}

	/* Step 5: wait for erase to complete */
	ret = mxl862xx_sb_pdi_poll_stat(priv, 0,
					MXL862XX_FW_ERASE_TIMEOUT_MS);
	if (ret) {
		dev_err(&priv->mdiodev->dev,
			"flash: erase timeout: %pe\n", ERR_PTR(ret));
		goto end_magic;
	}

	/* Step 6: transfer payload */
	mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_CTRL,
			     MXL862XX_SB_PDI_CTRL_WR);

	while (idx < payload_size) {
		if (idx + 1 < payload_size) {
			fdata = payload[idx] |
				((u16)payload[idx + 1] << 8);
			idx += 2;
			data_written += 2;
		} else {
			fdata = payload[idx];
			idx++;
			data_written++;
		}

		mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_DATA, fdata);
		word_idx++;

		if (idx >= payload_size) {
			ret = mxl862xx_sb_pdi_flush_slice(priv, data_written);
			break;
		}

		/* Half-bank boundary: switch to SB1 address */
		if (word_idx == MXL862XX_FW_BANK_HALF) {
			mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_CTRL,
					     MXL862XX_SB_PDI_CTRL_RST);
			mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_ADDR,
					     MXL862XX_FW_SB1_ADDR);
			mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_CTRL,
					     MXL862XX_SB_PDI_CTRL_WR);
		} else if (word_idx >= MXL862XX_FW_BANK_SLICE) {
			ret = mxl862xx_sb_pdi_flush_slice(priv, data_written);
			if (ret) {
				dev_err(&priv->mdiodev->dev,
					"flash: write timeout at %u/%u: %pe\n",
					idx, payload_size, ERR_PTR(ret));
				goto end_magic;
			}
			word_idx = 0;
			data_written = 0;
			mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_CTRL,
					     MXL862XX_SB_PDI_CTRL_WR);

			if (time_after(jiffies, next_notify)) {
				mxl862xx_flash_notify(dl, "Flashing", idx,
						      payload_size);
				next_notify = jiffies + msecs_to_jiffies(500);
			}
		}
	}

	if (ret) {
		dev_err(&priv->mdiodev->dev,
			"flash: final write timeout: %pe\n", ERR_PTR(ret));
		goto end_magic;
	}

	mxl862xx_flash_notify(dl, "Flashing", payload_size, payload_size);

end_magic:
	/* reboot MCUboot even after a failed transfer */
	mxl862xx_smdio_write(priv, MXL862XX_SB_PDI_STAT,
			     MXL862XX_SB_PDI_END);
	msleep(MXL862XX_FW_REBOOT_DELAY_MS);

	return ret;
}

int mxl862xx_devlink_info_get(struct dsa_switch *ds,
			      struct devlink_info_req *req,
			      struct netlink_ext_ack *extack)
{
	struct mxl862xx_priv *priv = ds->priv;
	const char *compatible;
	char ver_str[32];
	int ret;

	if (!of_property_read_string_index(ds->dev->of_node, "compatible", 0,
					   &compatible)) {
		ret = devlink_info_version_fixed_put(req,
						     DEVLINK_INFO_VERSION_GENERIC_ASIC_ID,
						     compatible);
		if (ret)
			return ret;
	}

	if (priv->rescue_mode)
		return devlink_info_version_running_put(req, "fw",
							"mcuboot-rescue");

	snprintf(ver_str, sizeof(ver_str), "%u.%u.%u",
		 priv->fw_version.major, priv->fw_version.minor,
		 priv->fw_version.revision);

	return devlink_info_version_running_put(req, "fw", ver_str);
}

int mxl862xx_devlink_flash_update(struct dsa_switch *ds,
				  struct devlink_flash_update_params *params,
				  struct netlink_ext_ack *extack)
{
	struct mxl862xx_priv *priv = ds->priv;
	struct mxl862xx_sys_fw_image_version ver = {};
	struct mxl862xx_reprobe *reprobe;
	struct dsa_port *dp;
	int ret, i;

	if (params->component) {
		NL_SET_ERR_MSG_MOD(extack, "component is not supported");
		return -EOPNOTSUPP;
	}

	if (priv->rescue_mode)
		dev_info(ds->dev,
			 "flash: recovering switch from MCUboot rescue mode\n");
	else
		dev_info(ds->dev, "flash: running firmware %u.%u.%u\n",
			 priv->fw_version.major, priv->fw_version.minor,
			 priv->fw_version.revision);

	/* Close ports while the firmware is still alive so the DSA
	 * core's MDB/FDB tracking is drained, and detach user ports
	 * so userspace cannot reopen them during the flash. The
	 * conduit belongs to the MAC driver and is only closed.
	 */
	rtnl_lock();
	dsa_switch_for_each_user_port(dp, ds) {
		if (dp->user) {
			dev_close(dp->user);
			netif_device_detach(dp->user);
		}
	}
	dsa_switch_for_each_cpu_port(dp, ds)
		dev_close(dp->conduit);
	rtnl_unlock();

	priv->block_host = true;

	cancel_delayed_work_sync(&priv->stats_work);
	for (i = 0; i < ds->num_ports; i++)
		cancel_work_sync(&priv->ports[i].host_flood_work);

	ret = mxl862xx_flash_firmware(priv, params->fw, ds->devlink);
	if (ret)
		NL_SET_ERR_MSG_MOD(extack, "firmware transfer failed");

	if (!ret) {
		/* lift the block to query the new firmware version */
		priv->block_host = false;
		priv->rescue_mode = false;
		memset(&ver, 0, sizeof(ver));
		if (!MXL862XX_API_READ_QUIET(priv, SYS_MISC_FW_VERSION, ver) &&
		    ver.iv_major)
			dev_info(ds->dev, "flash: new firmware %u.%u.%u\n",
				 ver.iv_major, ver.iv_minor,
				 le16_to_cpu(ver.iv_revision));
	}

	priv->skip_teardown = true;

	reprobe = kzalloc_obj(*reprobe);
	if (!reprobe)
		return ret;

	if (!try_module_get(THIS_MODULE)) {
		kfree(reprobe);
		return ret;
	}

	reprobe->dev = get_device(ds->dev);
	INIT_DELAYED_WORK(&reprobe->dwork, mxl862xx_reprobe_work_fn);
	schedule_delayed_work(&reprobe->dwork, msecs_to_jiffies(500));

	return ret;
}
