// SPDX-License-Identifier: GPL-2.0-only

/* CMIS loopback helpers for drivers implementing ethtool
 * get/set_loopback.
 *
 * Maps the generic ethtool loopback model to CMIS Page 13h registers
 * (CMIS 5.3, Table 8-128).
 *
 * Capabilities are read from Page 13h Byte 128, with Page 13h
 * availability checked via Page 01h Byte 142 bit 5.
 */

#include <linux/ethtool.h>
#include <linux/sfp.h>

#include "common.h"
#include "module_fw.h"
#include "cmis.h"

/* CMIS Page 00h, Byte 0: Physical module identifier */
#define CMIS_PHYS_ID_PAGE		0x00
#define CMIS_PHYS_ID_OFFSET		0x00

/* CMIS Page 01h, Byte 142: Diagnostic Pages Support */
#define CMIS_DIAG_SUPPORT_PAGE		0x01
#define CMIS_DIAG_SUPPORT_OFFSET	0x8e
#define CMIS_DIAG_PAGE13_BIT		BIT(5)

/* CMIS Page 13h, Byte 128: Loopback Capability Advertisement */
#define CMIS_LB_CAPS_PAGE		0x13
#define CMIS_LB_CAPS_OFFSET		0x80
#define CMIS_LB_CAP_MEDIA_OUTPUT	BIT(0)
#define CMIS_LB_CAP_MEDIA_INPUT		BIT(1)
#define CMIS_LB_CAP_HOST_OUTPUT		BIT(2)
#define CMIS_LB_CAP_HOST_INPUT		BIT(3)

/* CMIS Page 13h, Bytes 180-183: Per-Lane Loopback Control
 *   Byte 180 (0xb4): Media Side Output  -> MODULE, "cmis-media", remote
 *   Byte 181 (0xb5): Media Side Input   -> MODULE, "cmis-media", local
 *   Byte 182 (0xb6): Host Side Output   -> MODULE, "cmis-host",  remote
 *   Byte 183 (0xb7): Host Side Input    -> MODULE, "cmis-host",  local
 */
#define CMIS_LB_CTRL_PAGE		0x13
#define CMIS_LB_CTRL_OFFSET		0xb4
#define CMIS_LB_CTRL_LEN		4
#define CMIS_LB_CTRL_IDX_MEDIA_OUTPUT	0
#define CMIS_LB_CTRL_IDX_MEDIA_INPUT	1
#define CMIS_LB_CTRL_IDX_HOST_OUTPUT	2
#define CMIS_LB_CTRL_IDX_HOST_INPUT	3

#define CMIS_LB_NAME_HOST		"cmis-host"
#define CMIS_LB_NAME_MEDIA		"cmis-media"

static bool cmis_is_module(u8 phys_id)
{
	switch (phys_id) {
	case SFF8024_ID_QSFP_DD:
	case SFF8024_ID_OSFP:
	case SFF8024_ID_DSFP:
	case SFF8024_ID_QSFP_PLUS_CMIS:
	case SFF8024_ID_SFP_DD_CMIS:
	case SFF8024_ID_SFP_PLUS_CMIS:
		return true;
	default:
		return false;
	}
}

/**
 * cmis_loopback_caps - Read CMIS loopback capability mask
 * @dev: Network device
 *
 * Return: >0 capability bitmask, 0 if not a CMIS module or no Page
 *         13h, negative errno on failure.
 */
static int cmis_loopback_caps(struct net_device *dev)
{
	const struct ethtool_ops *ops = dev->ethtool_ops;
	struct ethtool_module_eeprom page = {};
	int ret;
	u8 val;

	if (!ops->get_module_eeprom_by_page)
		return 0;

	/* Read physical identifier */
	ethtool_cmis_page_init(&page, CMIS_PHYS_ID_PAGE,
			       CMIS_PHYS_ID_OFFSET, sizeof(val));
	page.data = &val;
	ret = ops->get_module_eeprom_by_page(dev, &page, NULL);
	if (ret < 0)
		return ret;
	if (!cmis_is_module(val))
		return 0;

	/* Check Page 13h availability */
	ethtool_cmis_page_init(&page, CMIS_DIAG_SUPPORT_PAGE,
			       CMIS_DIAG_SUPPORT_OFFSET, sizeof(val));
	page.data = &val;
	ret = ops->get_module_eeprom_by_page(dev, &page, NULL);
	if (ret < 0)
		return ret;
	if (!(val & CMIS_DIAG_PAGE13_BIT))
		return 0;

	/* Read capability byte */
	ethtool_cmis_page_init(&page, CMIS_LB_CAPS_PAGE,
			       CMIS_LB_CAPS_OFFSET, sizeof(val));
	page.data = &val;
	ret = ops->get_module_eeprom_by_page(dev, &page, NULL);
	if (ret < 0)
		return ret;

	return val & (CMIS_LB_CAP_MEDIA_OUTPUT | CMIS_LB_CAP_MEDIA_INPUT |
		      CMIS_LB_CAP_HOST_OUTPUT | CMIS_LB_CAP_HOST_INPUT);
}

/**
 * cmis_loopback_read - Read CMIS loopback capabilities and build entries
 * @dev: Network device with get_module_eeprom_by_page support
 * @host: Output host loopback entry (populated if host caps exist)
 * @media: Output media loopback entry (populated if media caps exist)
 * @has_host: Set to true if host loopback is supported
 * @has_media: Set to true if media loopback is supported
 *
 * Common helper that reads CMIS caps and control bytes, then populates
 * the host and media entries with current state.
 *
 * Return: 0 on success, -EOPNOTSUPP if no CMIS loopback support,
 *         negative errno on failure.
 */
static int cmis_loopback_read(struct net_device *dev,
			      struct ethtool_loopback_entry *host,
			      struct ethtool_loopback_entry *media,
			      bool *has_host, bool *has_media)
{
	const struct ethtool_ops *ops = dev->ethtool_ops;
	struct ethtool_module_eeprom page = {};
	u8 ctrl[CMIS_LB_CTRL_LEN];
	int caps, ret;

	*has_host = false;
	*has_media = false;

	if (dev->ethtool->module_fw_flash_in_progress)
		return -EBUSY;

	caps = cmis_loopback_caps(dev);
	if (caps <= 0)
		return caps ? caps : -EOPNOTSUPP;

	ethtool_cmis_page_init(&page, CMIS_LB_CTRL_PAGE,
			       CMIS_LB_CTRL_OFFSET, sizeof(ctrl));
	page.data = ctrl;
	ret = ops->get_module_eeprom_by_page(dev, &page, NULL);
	if (ret < 0)
		return ret;

	memset(host, 0, sizeof(*host));
	host->component = ETHTOOL_LOOPBACK_COMPONENT_MODULE;
	strscpy(host->name, CMIS_LB_NAME_HOST, sizeof(host->name));

	memset(media, 0, sizeof(*media));
	media->component = ETHTOOL_LOOPBACK_COMPONENT_MODULE;
	strscpy(media->name, CMIS_LB_NAME_MEDIA, sizeof(media->name));

	if (caps & CMIS_LB_CAP_HOST_INPUT) {
		*has_host = true;
		host->supported |= ETHTOOL_LOOPBACK_DIRECTION_LOCAL;
		if (ctrl[CMIS_LB_CTRL_IDX_HOST_INPUT])
			host->direction |= ETHTOOL_LOOPBACK_DIRECTION_LOCAL;
	}
	if (caps & CMIS_LB_CAP_HOST_OUTPUT) {
		*has_host = true;
		host->supported |= ETHTOOL_LOOPBACK_DIRECTION_REMOTE;
		if (ctrl[CMIS_LB_CTRL_IDX_HOST_OUTPUT])
			host->direction |= ETHTOOL_LOOPBACK_DIRECTION_REMOTE;
	}
	if (caps & CMIS_LB_CAP_MEDIA_INPUT) {
		*has_media = true;
		media->supported |= ETHTOOL_LOOPBACK_DIRECTION_LOCAL;
		if (ctrl[CMIS_LB_CTRL_IDX_MEDIA_INPUT])
			media->direction |= ETHTOOL_LOOPBACK_DIRECTION_LOCAL;
	}
	if (caps & CMIS_LB_CAP_MEDIA_OUTPUT) {
		*has_media = true;
		media->supported |= ETHTOOL_LOOPBACK_DIRECTION_REMOTE;
		if (ctrl[CMIS_LB_CTRL_IDX_MEDIA_OUTPUT])
			media->direction |= ETHTOOL_LOOPBACK_DIRECTION_REMOTE;
	}

	return 0;
}

/**
 * ethtool_cmis_get_loopback_by_index - Enumerate CMIS loopback entry by index
 * @dev: Network device with get_module_eeprom_by_page support
 * @index: Zero-based index of the loopback entry to retrieve
 * @entry: Output loopback entry
 *
 * Used by the dump infrastructure to iterate one entry at a time.
 *
 * Return: 0 on success, -EOPNOTSUPP if the index is out of range or
 *         no CMIS loopback support, negative errno on failure.
 */
int ethtool_cmis_get_loopback_by_index(struct net_device *dev, u32 index,
				       struct ethtool_loopback_entry *entry)
{
	struct ethtool_loopback_entry host, media;
	bool has_host, has_media;
	u32 cur = 0;
	int ret;

	ret = cmis_loopback_read(dev, &host, &media, &has_host, &has_media);
	if (ret)
		return ret;

	if (has_host) {
		if (cur == index) {
			memcpy(entry, &host, sizeof(*entry));
			return 0;
		}
		cur++;
	}

	if (has_media) {
		if (cur == index) {
			memcpy(entry, &media, sizeof(*entry));
			return 0;
		}
	}

	return -EOPNOTSUPP;
}

/**
 * ethtool_cmis_get_loopback - Look up CMIS loopback entry by name
 * @dev: Network device with get_module_eeprom_by_page support
 * @name: Loopback point name ("cmis-host" or "cmis-media")
 * @entry: Output loopback entry
 *
 * Used by doit requests to look up a specific loopback point.
 *
 * Return: 0 on success, -EOPNOTSUPP if name doesn't match or no CMIS
 *         support, negative errno on failure.
 */
int ethtool_cmis_get_loopback(struct net_device *dev,
			      const char *name,
			      struct ethtool_loopback_entry *entry)
{
	struct ethtool_loopback_entry host, media;
	bool has_host, has_media;
	int ret;

	ret = cmis_loopback_read(dev, &host, &media, &has_host, &has_media);
	if (ret)
		return ret;

	if (has_host && !strcmp(name, CMIS_LB_NAME_HOST)) {
		memcpy(entry, &host, sizeof(*entry));
		return 0;
	}

	if (has_media && !strcmp(name, CMIS_LB_NAME_MEDIA)) {
		memcpy(entry, &media, sizeof(*entry));
		return 0;
	}

	return -EOPNOTSUPP;
}

/**
 * ethtool_cmis_set_loopback - Apply one MODULE loopback entry to CMIS
 * @dev: Network device with get/set_module_eeprom_by_page support
 * @entry: Loopback entry to apply (must be MODULE component)
 * @extack: Netlink extended ack for error reporting
 *
 * Matches the entry against CMIS loopback points by name and
 * direction, then reads, modifies, and writes the corresponding Page
 * 13h control byte (0xff for all-lanes enable, 0x00 for disable).
 *
 * When disabling (direction == 0), all loopback points matching the
 * name are disabled regardless of their direction. When enabling,
 * only the specific direction is activated.
 *
 * Return: 1 if hardware state changed, 0 if already in requested state,
 *         negative errno on failure.
 */
int ethtool_cmis_set_loopback(struct net_device *dev,
			      const struct ethtool_loopback_entry *entry,
			      struct netlink_ext_ack *extack)
{
	struct ethtool_module_eeprom page = {};
	u8 ctrl[CMIS_LB_CTRL_LEN];
	int local_idx, remote_idx;
	u8 local_cap, remote_cap;
	bool mod = false;
	int caps, ret;

	if (!dev->ethtool_ops->set_module_eeprom_by_page) {
		NL_SET_ERR_MSG(extack,
			       "Module EEPROM write access not supported");
		return -EOPNOTSUPP;
	}

	if (dev->ethtool->module_fw_flash_in_progress) {
		NL_SET_ERR_MSG(extack,
			       "Module firmware flashing is in progress");
		return -EBUSY;
	}

	if (dev->flags & IFF_UP) {
		NL_SET_ERR_MSG(extack,
			       "Netdevice is up, module loopback change not permitted");
		return -EBUSY;
	}

	if (entry->direction && !is_power_of_2(entry->direction)) {
		NL_SET_ERR_MSG(extack,
			       "Only one loopback direction may be enabled at a time");
		return -EINVAL;
	}

	if (!strcmp(entry->name, CMIS_LB_NAME_HOST)) {
		local_idx = CMIS_LB_CTRL_IDX_HOST_INPUT;
		remote_idx = CMIS_LB_CTRL_IDX_HOST_OUTPUT;
		local_cap = CMIS_LB_CAP_HOST_INPUT;
		remote_cap = CMIS_LB_CAP_HOST_OUTPUT;
	} else if (!strcmp(entry->name, CMIS_LB_NAME_MEDIA)) {
		local_idx = CMIS_LB_CTRL_IDX_MEDIA_INPUT;
		remote_idx = CMIS_LB_CTRL_IDX_MEDIA_OUTPUT;
		local_cap = CMIS_LB_CAP_MEDIA_INPUT;
		remote_cap = CMIS_LB_CAP_MEDIA_OUTPUT;
	} else {
		NL_SET_ERR_MSG(extack, "Unknown CMIS loopback name");
		return -EINVAL;
	}

	caps = cmis_loopback_caps(dev);
	if (caps < 0)
		return caps;
	if (!caps) {
		NL_SET_ERR_MSG(extack, "Module does not support CMIS loopback");
		return -EOPNOTSUPP;
	}

	/* Read current control bytes */
	ethtool_cmis_page_init(&page, CMIS_LB_CTRL_PAGE,
			       CMIS_LB_CTRL_OFFSET, sizeof(ctrl));
	page.data = ctrl;
	ret = dev->ethtool_ops->get_module_eeprom_by_page(dev, &page, NULL);
	if (ret < 0)
		return ret;

	if (!entry->direction) {
		/* Disable both directions */
		if (ctrl[local_idx]) {
			ctrl[local_idx] = 0x00;
			mod = true;
		}
		if (ctrl[remote_idx]) {
			ctrl[remote_idx] = 0x00;
			mod = true;
		}
	} else {
		int enable_idx, disable_idx;
		u8 enable_cap;

		if (entry->direction & ETHTOOL_LOOPBACK_DIRECTION_LOCAL) {
			enable_idx = local_idx;
			enable_cap = local_cap;
			disable_idx = remote_idx;
		} else {
			enable_idx = remote_idx;
			enable_cap = remote_cap;
			disable_idx = local_idx;
		}

		if (!(caps & enable_cap)) {
			NL_SET_ERR_MSG(extack,
				       "Loopback mode not supported by module");
			return -EOPNOTSUPP;
		}

		/* Disable opposite direction first (mutual exclusivity) */
		if (ctrl[disable_idx]) {
			ctrl[disable_idx] = 0x00;
			ret = dev->ethtool_ops->set_module_eeprom_by_page(dev,
				&page, extack);
			if (ret < 0)
				return ret;
			mod = true;
		}

		if (ctrl[enable_idx] != 0xff) {
			ctrl[enable_idx] = 0xff;
			mod = true;
		}
	}

	if (!mod)
		return 0;

	ret = dev->ethtool_ops->set_module_eeprom_by_page(dev, &page, extack);

	return ret < 0 ? ret : 1;
}
