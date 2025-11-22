// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * phy-common-props.c  --  Common PHY properties
 *
 * Copyright 2025 NXP
 */
#include <linux/export.h>
#include <linux/fwnode.h>
#include <linux/phy/phy-common-props.h>
#include <linux/printk.h>
#include <linux/property.h>
#include <linux/slab.h>

static int phy_get_polarity_for_mode(struct fwnode_handle *fwnode,
				     const char *mode_name,
				     unsigned int supported,
				     unsigned int default_val,
				     const char *polarity_prop,
				     const char *names_prop)
{
	int err, n_pols, n_names, idx = -1;
	u32 val, *pols;

	if (!fwnode)
		return default_val;

	n_pols = fwnode_property_count_u32(fwnode, polarity_prop);
	if (n_pols <= 0)
		return default_val;

	n_names = fwnode_property_string_array_count(fwnode, names_prop);
	if (n_names >= 0 && n_pols != n_names) {
		pr_err("%pfw mismatch between \"%s\" and \"%s\" property count (%d vs %d)\n",
		       fwnode, polarity_prop, names_prop, n_pols, n_names);
		return -EINVAL;
	}

	if (mode_name)
		idx = fwnode_property_match_string(fwnode, names_prop, mode_name);
	if (idx < 0)
		idx = fwnode_property_match_string(fwnode, names_prop, "default");
	/*
	 * If the mode name is missing, it can only mean the specified polarity
	 * is the default one for all modes, so reject any other polarity count
	 * than 1.
	 */
	if (idx < 0 && n_pols != 1) {
		pr_err("%pfw \"%s \" property has %d elements, but cannot find \"%s\" in \"%s\" and there is no default value\n",
		       fwnode, polarity_prop, n_pols, mode_name, names_prop);
		return -EINVAL;
	}

	if (n_pols == 1) {
		err = fwnode_property_read_u32(fwnode, polarity_prop, &val);
		if (err)
			return err;

		return val;
	}

	/* We implicitly know idx >= 0 here */
	pols = kcalloc(n_pols, sizeof(*pols), GFP_KERNEL);
	if (!pols)
		return -ENOMEM;

	err = fwnode_property_read_u32_array(fwnode, polarity_prop, pols, n_pols);
	if (err == 0) {
		val = pols[idx];
		if (!(supported & BIT(val))) {
			pr_err("%pfw mismatch between '%s' and '%s' property count (%d vs %d)\n",
			       fwnode, polarity_prop, names_prop, n_pols, n_names);
			err = -EOPNOTSUPP;
		}
	}

	kfree(pols);

	return (err < 0) ? err : val;
}

/**
 * phy_get_rx_polarity - Get RX polarity for PHY differential lane
 * @fwnode: Pointer to the PHY's firmware node.
 * @mode_name: The name of the PHY mode to look up.
 * @supported: Bit mask of PHY_POL_NORMAL, PHY_POL_INVERT and PHY_POL_AUTO
 * @default_val: Default polarity value if property is missing
 *
 * Return: One of PHY_POL_NORMAL, PHY_POL_INVERT or PHY_POL_AUTO on success, or
 *	   negative error on failure.
 */
int phy_get_rx_polarity(struct fwnode_handle *fwnode, const char *mode_name,
			unsigned int supported, unsigned int default_val)
{
	return phy_get_polarity_for_mode(fwnode, mode_name, supported,
					 default_val, "rx-polarity",
					 "rx-polarity-names");
}
EXPORT_SYMBOL_GPL(phy_get_rx_polarity);

/**
 * phy_get_tx_polarity - Get TX polarity for PHY differential lane
 * @fwnode: Pointer to the PHY's firmware node.
 * @mode_name: The name of the PHY mode to look up.
 * @supported: Bit mask of PHY_POL_NORMAL and PHY_POL_INVERT
 * @default_val: Default polarity value if property is missing
 *
 * Return: One of PHY_POL_NORMAL or PHY_POL_INVERT on success, or negative
 *	   error on failure.
 */
int phy_get_tx_polarity(struct fwnode_handle *fwnode, const char *mode_name,
			unsigned int supported, unsigned int default_val)
{
	return phy_get_polarity_for_mode(fwnode, mode_name, supported,
					 default_val, "tx-polarity",
					 "tx-polarity-names");
}
EXPORT_SYMBOL_GPL(phy_get_tx_polarity);
