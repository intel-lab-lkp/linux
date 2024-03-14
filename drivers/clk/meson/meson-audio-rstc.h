/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */

#ifndef __MESON_AUDIO_RSTC_H
#define __MESON_AUDIO_RSTC_H

#include <linux/device.h>
#include <linux/regmap.h>

int meson_audio_rstc_register(struct device *dev, struct regmap *map,
			      unsigned int offset, unsigned int num);

#endif /* __MESON_AUDIO_RSTC_H */
