/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_RESET_SKY1_H__
#define __LINUX_RESET_SKY1_H__

struct sky1_src_signal {
	unsigned int offset;
	unsigned int bit;
};

struct sky1_src_variant {
	const struct sky1_src_signal *signals;
	unsigned int signals_num;
};

int sky1_reset_common_probe(struct platform_device *pdev,
			    const struct sky1_src_variant *variant);

#endif /* __LINUX_RESET_SKY1_H__ */
