/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021 Emil Renner Berthing <kernel@esmil.dk>
 */

#ifndef __RESET_STARFIVE_JH71X0_H
#define __RESET_STARFIVE_JH71X0_H

struct jh71x0_reset_map {
	unsigned long reset_id;
	unsigned long hw_id;
};

struct jh71x0_reset_info {
	unsigned int nr_resets;
	unsigned int assert_offset;
	unsigned int status_offset;
	bool discontiguous;
	const struct jh71x0_reset_map *map;
};

int reset_starfive_jh71x0_register(struct device *dev, struct device_node *of_node,
				   void __iomem *assert, void __iomem *status,
				   const u32 *asserted, unsigned int nr_resets,
				   struct module *owner);

int reset_starfive_jh71x0_register_with_info(struct device *dev, struct device_node *of_node,
					     void __iomem *assert, void __iomem *status,
					     const u32 *asserted,
					     const struct jh71x0_reset_info *info,
					     struct module *owner);

#endif /* __RESET_STARFIVE_JH71X0_H */
