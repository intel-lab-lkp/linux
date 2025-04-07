/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 Sean Anderson <sean.anderson@seco.com>
 */

#ifndef _PCS_H
#define _PCS_H

#include <linux/fwnode.h>

struct device_node;
struct of_changeset;
struct phylink_pcs;

/**
 * typedef pcs_xlate_t - Translate reference arguments to a PCS
 * @args: An array of arguments to the reference
 * @nargs: Length of @args
 * @data: Private pointer passed to pcs_register_provider()
 *
 * Look up a PCS based on @args. These are typically passed from a devicetree
 * reference. For example, a devicetree like::
 *
 *     pcs: my-pcs {
 *         #pcs-cells = <2>;
 *     };
 *
 *     mac {
 *       pcs = <&pcs 5 98>;
 *       pcs-names = "sfi";
 *     };
 *
 * would result in this function getting called with @args = ``{ 5, 98 }``.
 *
 * This function should perform a simple translation only. It should not
 * allocate memory or perform I/O.
 *
 * Return: A PCS or %NULL if @args cannot be translated.
 */
typedef struct phylink_pcs *(*pcs_xlate_t)(const u64 *args, unsigned int nargs, void *data);

struct phylink_pcs *pcs_xlate_single(const u64 *args, unsigned int nargs,
				     void *data);

int pcs_register_provider(struct device *dev, pcs_xlate_t xlate, void *data);
void pcs_unregister_provider(struct device *dev);
int devm_pcs_register_provider(struct device *dev, pcs_xlate_t xlate,
			       void *data);

int pcs_find_fwnode(const struct fwnode_handle *mac_node, const char *id,
		    const char *fallback, bool optional,
		    struct fwnode_reference_args *args);
struct phylink_pcs *
pcs_get_by_fwnode_compat(struct device *dev, struct fwnode_handle *fwnode,
			 int (*fixup)(struct of_changeset *ocs,
				      struct device_node *np, void *data),
			 void *data);

#ifdef CONFIG_PCS
struct phylink_pcs *_pcs_get_tail(struct device *dev,
				  const struct fwnode_reference_args *args,
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
	struct fwnode_reference_args args;

	args.fwnode = NULL;
	return _pcs_get_tail(dev, &args, pcs_dev);
}
#else /* CONFIG_PCS */
static inline void pcs_put(struct device *dev, struct phylink_pcs *handle)
{
}

static inline struct phylink_pcs *pcs_get(struct device *dev, const char *id)
{
	return -EOPNOTSUPP;
}

static inline struct phylink_pcs *pcs_get_optional(struct device *dev,
						   const char *id)
{
	return NULL;
}

static inline struct phylink_pcs
*pcs_get_by_fwnode(struct device *dev, struct fwnode_handle *fwnode,
		   const char *id)
{
	return -EOPNOTSUPP;
}

static inline struct phylink_pcs
*pcs_get_by_fwnode_optional(struct device *dev, struct fwnode_handle *fwnode,
			    const char *id)
{
	return NULL;
}

static inline struct phylink_pcs *pcs_get_by_dev(struct device *dev,
						 const struct device *pcs_dev)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* PCS_H */
