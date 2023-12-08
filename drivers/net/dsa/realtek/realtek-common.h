/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _REALTEK_INTERFACE_H
#define _REALTEK_INTERFACE_H

#include <linux/regmap.h>

void realtek_common_lock(void *ctx);
void realtek_common_unlock(void *ctx);
struct realtek_priv *
realtek_common_probe_pre(struct device *dev, struct regmap_config rc,
			 struct regmap_config rc_nolock);
int realtek_common_probe_post(struct realtek_priv *priv);
void realtek_common_remove(struct realtek_priv *priv);

#endif
