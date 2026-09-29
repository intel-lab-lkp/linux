/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SL261X_PLL_H
#define __SL261X_PLL_H

#include <linux/io.h>

struct clk_hw_onecell_data;
struct device;
struct device_node;

int sl261x_pll_register(struct device *dev, void __iomem *ctrl,
			const char *out, const char *outf);

#endif /* __SL261X_PLL_H */
