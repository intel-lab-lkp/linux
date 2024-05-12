/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ENS160_H_
#define ENS160_H_

int ens160_core_probe(struct device *dev, struct regmap *regmap,
		      const char *name);
void ens160_core_remove(struct device *dev);

#endif
