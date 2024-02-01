/* SPDX-License-Identifier: GPL-2.0-only */

#include <uapi/linux/ethtool.h>

void ethnl_module_fw_flash_ntf_err(struct net_device *dev,
				   const char *status_msg);
void ethnl_module_fw_flash_ntf_start(struct net_device *dev);
void ethnl_module_fw_flash_ntf_complete(struct net_device *dev);
void ethnl_module_fw_flash_ntf_in_progress(struct net_device *dev, u64 done,
					   u64 total);
