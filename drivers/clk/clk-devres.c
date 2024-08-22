// SPDX-License-Identifier: GPL-2.0
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/types.h>

struct devm_clk_state {
	struct clk *clk;
	void (*exit)(struct clk *clk);
};

static void devm_clk_release(void *res)
{
	struct devm_clk_state *state = res;

	if (state->exit)
		state->exit(state->clk);

	clk_put(state->clk);
}

static struct clk *__devm_clk_get(struct device *dev, const char *id,
				  struct clk *(*get)(struct device *dev, const char *id),
				  int (*init)(struct clk *clk),
				  void (*exit)(struct clk *clk))
{
	struct devm_clk_state *state;
	struct clk *clk;
	int ret;

	state = devm_kmalloc(dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return ERR_PTR(-ENOMEM);

	clk = get(dev, id);
	if (IS_ERR(clk))
		return clk;

	if (init) {
		ret = init(clk);
		if (ret)
			goto err_clk_init;
	}

	state->clk = clk;
	state->exit = exit;

	ret = devm_add_action_or_reset(dev, devm_clk_release, state);
	if (ret)
		goto err_clk_init;

	return clk;

err_clk_init:
	clk_put(clk);
	return ERR_PTR(ret);
}

struct clk *devm_clk_get(struct device *dev, const char *id)
{
	return __devm_clk_get(dev, id, clk_get, NULL, NULL);
}
EXPORT_SYMBOL(devm_clk_get);

struct clk *devm_clk_get_prepared(struct device *dev, const char *id)
{
	return __devm_clk_get(dev, id, clk_get, clk_prepare, clk_unprepare);
}
EXPORT_SYMBOL_GPL(devm_clk_get_prepared);

struct clk *devm_clk_get_enabled(struct device *dev, const char *id)
{
	return __devm_clk_get(dev, id, clk_get,
			      clk_prepare_enable, clk_disable_unprepare);
}
EXPORT_SYMBOL_GPL(devm_clk_get_enabled);

struct clk *devm_clk_get_optional(struct device *dev, const char *id)
{
	return __devm_clk_get(dev, id, clk_get_optional, NULL, NULL);
}
EXPORT_SYMBOL(devm_clk_get_optional);

struct clk *devm_clk_get_optional_prepared(struct device *dev, const char *id)
{
	return __devm_clk_get(dev, id, clk_get_optional,
			      clk_prepare, clk_unprepare);
}
EXPORT_SYMBOL_GPL(devm_clk_get_optional_prepared);

struct clk *devm_clk_get_optional_enabled(struct device *dev, const char *id)
{
	return __devm_clk_get(dev, id, clk_get_optional,
			      clk_prepare_enable, clk_disable_unprepare);
}
EXPORT_SYMBOL_GPL(devm_clk_get_optional_enabled);

struct clk_bulk_devres {
	struct clk_bulk_data *clks;
	int num_clks;
};

static void devm_clk_bulk_release(void *res)
{
	struct clk_bulk_devres *devres = res;

	clk_bulk_put(devres->num_clks, devres->clks);
}

static int __devm_clk_bulk_get(struct device *dev, int num_clks,
			       struct clk_bulk_data *clks, bool optional)
{
	struct clk_bulk_devres *devres;
	int ret;

	devres = devm_kmalloc(dev, sizeof(*devres), GFP_KERNEL);
	if (!devres)
		return -ENOMEM;

	if (optional)
		ret = clk_bulk_get_optional(dev, num_clks, clks);
	else
		ret = clk_bulk_get(dev, num_clks, clks);
	if (ret)
		return ret;

	devres->clks = clks;
	devres->num_clks = num_clks;

	return devm_add_action_or_reset(dev, devm_clk_bulk_release, devres);
}

int __must_check devm_clk_bulk_get(struct device *dev, int num_clks,
		      struct clk_bulk_data *clks)
{
	return __devm_clk_bulk_get(dev, num_clks, clks, false);
}
EXPORT_SYMBOL_GPL(devm_clk_bulk_get);

int __must_check devm_clk_bulk_get_optional(struct device *dev, int num_clks,
		      struct clk_bulk_data *clks)
{
	return __devm_clk_bulk_get(dev, num_clks, clks, true);
}
EXPORT_SYMBOL_GPL(devm_clk_bulk_get_optional);

static void devm_clk_bulk_release_all(void *res)
{
	struct clk_bulk_devres *devres = res;

	clk_bulk_put_all(devres->num_clks, devres->clks);
}

int __must_check devm_clk_bulk_get_all(struct device *dev,
				       struct clk_bulk_data **clks)
{
	struct clk_bulk_devres *devres;
	int ret;

	devres = devm_kmalloc(dev, sizeof(*devres), GFP_KERNEL);
	if (!devres)
		return -ENOMEM;

	devres->num_clks = clk_bulk_get_all(dev, &devres->clks);
	if (devres->num_clks <= 0)
		return devres->num_clks;

	ret = devm_add_action_or_reset(dev, devm_clk_bulk_release_all, devres);
	if (ret)
		return ret;

	*clks = devres->clks;
	return 0;
}
EXPORT_SYMBOL_GPL(devm_clk_bulk_get_all);

static void devm_clk_bulk_release_all_enable(void *res)
{
	struct clk_bulk_devres *devres = res;

	clk_bulk_disable_unprepare(devres->num_clks, devres->clks);
	clk_bulk_put_all(devres->num_clks, devres->clks);
}

int __must_check devm_clk_bulk_get_all_enable(struct device *dev,
					      struct clk_bulk_data **clks)
{
	struct clk_bulk_devres *devres;
	int ret;

	devres = devm_kmalloc(dev, sizeof(*devres), GFP_KERNEL);
	if (!devres)
		return -ENOMEM;

	devres->num_clks = clk_bulk_get_all(dev, &devres->clks);
	if (devres->num_clks <= 0)
		return devres->num_clks;

	ret = clk_bulk_prepare_enable(devres->num_clks, devres->clks);
	if (ret) {
		clk_bulk_put_all(devres->num_clks, devres->clks);
		return ret;
	}

	ret = devm_add_action_or_reset(dev, devm_clk_bulk_release_all_enable, devres);
	if (ret)
		return ret;

	*clks = devres->clks;
	return 0;

}
EXPORT_SYMBOL_GPL(devm_clk_bulk_get_all_enable);

void devm_clk_put(struct device *dev, struct clk *clk)
{
	devm_release_action(dev, devm_clk_release, clk);
}
EXPORT_SYMBOL(devm_clk_put);

struct clk *devm_get_clk_from_child(struct device *dev,
				    struct device_node *np, const char *con_id)
{
	struct devm_clk_state *state;
	int ret;

	state = devm_kmalloc(dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return ERR_PTR(-ENOMEM);

	state->clk = of_clk_get_by_name(np, con_id);
	if (IS_ERR(state->clk))
		return state->clk;

	ret = devm_add_action_or_reset(dev, devm_clk_release, state);
	if (ret)
		return ERR_PTR(ret);

	return state->clk;
}
EXPORT_SYMBOL(devm_get_clk_from_child);
