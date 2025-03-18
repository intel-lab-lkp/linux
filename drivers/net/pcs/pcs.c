// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phylink.h>
#include <linux/pcs/pcs.h>
#include <linux/pcs/pcs-provider.h>

struct of_pcs_provider {
	struct list_head link;

	struct device_node *node;
	struct phylink_pcs *(*get)(struct of_phandle_args *pcsspec,
				   void *data,
				   phy_interface_t interface);

	void *data;
};

static LIST_HEAD(of_pcs_providers);
static DEFINE_MUTEX(of_pcs_mutex);

struct phylink_pcs *of_pcs_simple_get(struct of_phandle_args *pcsspec, void *data,
				      phy_interface_t interface)
{
	struct phylink_pcs *pcs = data;

	if (!pcs_supports_interface(pcs, interface))
		return ERR_PTR(-EOPNOTSUPP);

	return data;
}
EXPORT_SYMBOL_GPL(of_pcs_simple_get);

int of_pcs_add_provider(struct device_node *np,
			struct phylink_pcs *(*get)(struct of_phandle_args *pcsspec,
						   void *data,
						   phy_interface_t interface),
			void *data)
{
	struct of_pcs_provider *pp;

	if (!np)
		return 0;

	pp = kzalloc(sizeof(*pp), GFP_KERNEL);
	if (!pp)
		return -ENOMEM;

	pp->node = of_node_get(np);
	pp->data = data;
	pp->get = get;

	mutex_lock(&of_pcs_mutex);
	list_add(&pp->link, &of_pcs_providers);
	mutex_unlock(&of_pcs_mutex);
	pr_debug("Added pcs provider from %pOF\n", np);

	fwnode_dev_initialized(&np->fwnode, true);

	return 0;
}
EXPORT_SYMBOL_GPL(of_pcs_add_provider);

void of_pcs_del_provider(struct device_node *np)
{
	struct of_pcs_provider *pp;

	if (!np)
		return;

	mutex_lock(&of_pcs_mutex);
	list_for_each_entry(pp, &of_pcs_providers, link) {
		if (pp->node == np) {
			list_del(&pp->link);
			fwnode_dev_initialized(&np->fwnode, false);
			of_node_put(pp->node);
			kfree(pp);
			break;
		}
	}
	mutex_unlock(&of_pcs_mutex);
}
EXPORT_SYMBOL_GPL(of_pcs_del_provider);

static int of_parse_pcsspec(const struct device_node *np, int index,
			    const char *name, struct of_phandle_args *out_args)
{
	int ret = -ENOENT;

	if (!np)
		return -ENOENT;

	if (name)
		index = of_property_match_string(np, "pcs-names", name);

	ret = of_parse_phandle_with_args(np, "pcs-handle", "#pcs-cells",
					 index, out_args);
	if (ret || (name && index < 0))
		return ret;

	return 0;
}

static struct phylink_pcs *
of_pcs_get_from_pcsspec(struct of_phandle_args *pcsspec,
			phy_interface_t interface)
{
	struct of_pcs_provider *provider;
	struct phylink_pcs *pcs = ERR_PTR(-EPROBE_DEFER);

	if (!pcsspec)
		return ERR_PTR(-EINVAL);

	mutex_lock(&of_pcs_mutex);
	list_for_each_entry(provider, &of_pcs_providers, link) {
		if (provider->node == pcsspec->np) {
			pcs = provider->get(pcsspec, provider->data,
					    interface);
			if (!IS_ERR(pcs))
				break;
		}
	}
	mutex_unlock(&of_pcs_mutex);

	return pcs;
}

static struct phylink_pcs *__of_pcs_get(struct device_node *np, int index,
					const char *con_id,
					phy_interface_t interface)
{
	struct of_phandle_args pcsspec;
	struct phylink_pcs *pcs;
	int ret;

	ret = of_parse_pcsspec(np, index, con_id, &pcsspec);
	if (ret)
		return ERR_PTR(ret);

	pcs = of_pcs_get_from_pcsspec(&pcsspec, interface);
	of_node_put(pcsspec.np);

	return pcs;
}

struct phylink_pcs *of_pcs_get(struct device_node *np, int index,
			       phy_interface_t interface)
{
	return __of_pcs_get(np, index, NULL, interface);
}
EXPORT_SYMBOL_GPL(of_pcs_get);

struct phylink_pcs *of_phylink_mac_select_pcs(struct phylink_config *config,
					      phy_interface_t interface)
{
	int i, count;
	struct device *dev = config->dev;
	struct device_node *np = dev->of_node;
	struct phylink_pcs *pcs = ERR_PTR(-ENODEV);

	/* To enable using_mac_select_pcs on phylink_create */
	if (interface == PHY_INTERFACE_MODE_NA)
		return NULL;

	/* Reject configuring PCS with Internal mode */
	if (interface == PHY_INTERFACE_MODE_INTERNAL)
		return ERR_PTR(-EINVAL);

	if (!of_property_present(np, "pcs-handle"))
		return pcs;

	count = of_count_phandle_with_args(np, "pcs-handle", "#pcs-cells");
	if (count < 0)
		return ERR_PTR(count);

	for (i = 0; i < count; i++) {
		pcs = of_pcs_get(np, i, interface);
		if (!IS_ERR_OR_NULL(pcs))
			return pcs;
	}

	return pcs;
}
EXPORT_SYMBOL_GPL(of_phylink_mac_select_pcs);
