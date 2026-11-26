// SPDX-License-Identifier: GPL-2.0

#include <linux/reset.h>

__rust_helper struct reset_control *
rust_helper_reset_control_get_exclusive(struct device *dev, const char *id)
{
	return reset_control_get_exclusive(dev, id);
}

__rust_helper struct reset_control *
rust_helper_reset_control_get_optional_exclusive(struct device *dev,
						 const char *id)
{
	return reset_control_get_optional_exclusive(dev, id);
}

#ifndef CONFIG_RESET_CONTROLLER
__rust_helper struct reset_control *
rust_helper_fwnode_reset_control_array_get(struct fwnode_handle *fwnode,
					   enum reset_control_flags flags)
{
	return fwnode_reset_control_array_get(fwnode, flags);
}

__rust_helper void rust_helper_reset_control_put(struct reset_control *rstc)
{
	reset_control_put(rstc);
}

__rust_helper int rust_helper_reset_control_assert(struct reset_control *rstc)
{
	return reset_control_assert(rstc);
}

__rust_helper int rust_helper_reset_control_deassert(struct reset_control *rstc)
{
	return reset_control_deassert(rstc);
}
#endif
