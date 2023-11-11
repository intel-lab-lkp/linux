/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _REALTEK_INTERFACE_H
#define _REALTEK_INTERFACE_H

#include <linux/regmap.h>

extern const struct of_device_id realtek_common_of_match[];

void realtek_common_lock(void *ctx);
void realtek_common_unlock(void *ctx);
struct realtek_priv *realtek_common_probe(struct device *dev,
		struct regmap_config rc, struct regmap_config rc_nolock);
void realtek_common_remove(struct realtek_priv *priv);

#endif
