/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 Sean Anderson <sean.anderson@seco.com>
 */

#ifndef _PCS_H
#define _PCS_H

struct device_node;
struct of_changeset;
struct phylink_pcs;

int pcs_register(struct device *dev, struct phylink_pcs *pcs);
void pcs_unregister(struct phylink_pcs *pcs);
int devm_pcs_register(struct device *dev, struct phylink_pcs *pcs);
struct phylink_pcs *_pcs_get_tail(struct device *dev,
				  const struct fwnode_handle *fwnode,
				  const struct device *pcs_dev);
struct phylink_pcs *_pcs_get(struct device *dev, struct fwnode_handle *fwnode,
			     const char *id, const char *fallback,
			     bool optional);
void pcs_put(struct device *dev, struct phylink_pcs *handle);

/**
 * pcs_get() - Get a PCS based on a fwnode
 * @dev: The device requesting the PCS
 * @id: The name of the PCS
 *
 * Find and get a PCS, as referenced by @dev's &struct fwnode_handle. See
 * pcs_find_fwnode() for details. Each call to this function must be balanced
 * with one to pcs_put().
 *
 * Return: A PCS on success or an error pointer on failure
 */
static inline struct phylink_pcs *pcs_get(struct device *dev, const char *id)
{
	return _pcs_get(dev, dev_fwnode(dev), id, NULL, false);
}

/**
 * pcs_get_optional() - Optionally get a PCS based on a fwnode
 * @dev: The device requesting the PCS
 * @id: The name of the PCS
 *
 * Optionally find and get a PCS, as referenced by @dev's &struct
 * fwnode_handle. See pcs_find_fwnode() for details. Each call to this function
 * must be balanced with one to pcs_put().
 *
 * Return: A PCS on success, %NULL if none was found, or an error pointer on
 * *       failure
 */
static inline struct phylink_pcs *pcs_get_optional(struct device *dev,
						   const char *id)
{
	return _pcs_get(dev, dev_fwnode(dev), id, NULL, true);
}

/**
 * pcs_get_by_fwnode() - Get a PCS based on a fwnode
 * @dev: The device requesting the PCS
 * @fwnode: The &struct fwnode_handle referencing the PCS
 * @id: The name of the PCS
 *
 * Find and get a PCS, as referenced by @fwnode. See pcs_find_fwnode() for
 * details. Each call to this function must be balanced with one to pcs_put().
 *
 * Return: A PCS on success or an error pointer on failure
 */
static inline struct phylink_pcs
*pcs_get_by_fwnode(struct device *dev, struct fwnode_handle *fwnode,
		   const char *id)
{
	return _pcs_get(dev, fwnode, id, NULL, false);
}

/**
 * pcs_get_by_fwnode_optional() - Optionally get a PCS based on a fwnode
 * @dev: The device requesting the PCS
 * @fwnode: The &struct fwnode_handle referencing the PCS
 * @id: The name of the PCS
 *
 * Optionally find and get a PCS, as referenced by @fwnode. See
 * pcs_find_fwnode() for details. Each call to this function must be balanced
 * with one to pcs_put().
 *
 * Return: A PCS on success, %NULL if none was found, or an error pointer on
 * *       failure
 */
static inline struct phylink_pcs
*pcs_get_by_fwnode_optional(struct device *dev, struct fwnode_handle *fwnode,
			    const char *id)
{
	return _pcs_get(dev, fwnode, id, NULL, true);
}

/**
 * pcs_get_by_dev() - Get a PCS from its providing device
 * @dev: The device requesting the PCS
 * @pcs_dev: The device providing the PCS
 *
 * Get the first PCS registered by @pcs_dev. Each call to this function must be
 * balanced with one to pcs_put().
 *
 * Return: A PCS on success or an error pointer on failure
 */
static inline struct phylink_pcs *pcs_get_by_dev(struct device *dev,
						 const struct device *pcs_dev)
{
	return _pcs_get_tail(dev, NULL, pcs_dev);
}

struct fwnode_handle *pcs_find_fwnode(const struct fwnode_handle *mac_node,
				      const char *id, const char *fallback,
				      bool optional);

struct phylink_pcs *
pcs_get_by_fwnode_compat(struct device *dev, struct fwnode_handle *fwnode,
			 int (*fixup)(struct of_changeset *ocs, struct device_node *np,
				      void *data),
			 void *data);

#endif /* PCS_H */
