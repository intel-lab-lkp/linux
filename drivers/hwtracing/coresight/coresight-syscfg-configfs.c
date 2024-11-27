// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Linaro Limited, All rights reserved.
 * Author: Mike Leach <mike.leach@linaro.org>
 */

#include <linux/configfs.h>
#include <linux/module.h>
#include <linux/workqueue.h>

#include "coresight-config.h"
#include "coresight-config-table.h"
#include "coresight-syscfg-configfs.h"

/* prevent race in load / unload operations */
static DEFINE_MUTEX(cfs_mutex);

/*
 * need to enable / disable dynamic table load when
 * initialising / shutting down the subsystem, or
 * loading / unloading configurations via module.
 */
static bool cscfg_dyn_load_enabled;

/*
 * Lockdep issues occur if deleting the config directory as part
 * of the unload operation triggered by configfs.
 * Therefore we schedule the main part of the unload to be completed as a work item
 * & save the owner info for the scheduled unload
 */
static struct cscfg_load_owner_info *cscfg_sched_dyn_unload_owner;


/* determine if load / unload ops are currently permitted. */
inline bool cscfg_load_ops_permitted(void)
{
	return (cscfg_dyn_load_enabled && !cscfg_sched_dyn_unload_owner);
}

/* do the main unload operations. Called with cfs_mutex held */
static int cscfg_do_unload(struct cscfg_load_owner_info *unload_owner)
{
	int err = 0;

	if (!cscfg_dyn_load_enabled) {
		pr_warn("cscfg: skipping unload completion\n");
		return -EINVAL;
	}

	err = cscfg_unload_config_sets(unload_owner);
	if (!err)
		cscfg_free_dyn_load_owner_info(unload_owner);
	else
		pr_err("cscfg: dynamic configuration unload error\n");

	return err;
}

/* complete the unload operation as work item  */
static void cscfg_complete_unload(struct work_struct *work)
{
	mutex_lock(&cfs_mutex);

	if (cscfg_sched_dyn_unload_owner)
		cscfg_do_unload(cscfg_sched_dyn_unload_owner);
	cscfg_sched_dyn_unload_owner = NULL;

	mutex_unlock(&cfs_mutex);
	kfree(work);
}

static int cscfg_schedule_unload(void)
{
	struct work_struct *work;

	work = kzalloc(sizeof(struct work_struct), GFP_KERNEL);
	if (!work)
		return -ENOMEM;

	INIT_WORK(work, cscfg_complete_unload);
	schedule_work(work);
	return 0;
}

/* create a string representing a loaded config based on owner info */
static ssize_t cscfg_get_owner_info_str(struct cscfg_load_owner_info *owner_info,
					char *buffer, ssize_t size)
{
	struct cscfg_table_load_descs *load_descs;
	ssize_t size_used = 0;
	int i;
	static const char * const load_type[] = {
		"Built in driver",
		"Loadable module",
		"Runtime Dynamic table load",
	};

	/* limited info for none dynamic loaded stuff */
	if (owner_info->type != CSCFG_OWNER_DYNLOAD) {
		size_used = scnprintf(buffer, size,
				      "load name: [Not Set]\nload type: %s\n",
				      load_type[owner_info->type]);
		goto buffer_done;
	}

	/*  dynamic loaded type will have all the info */
	load_descs = (struct cscfg_table_load_descs *)owner_info->owner_handle;

	/* first is the load name and type - need for unload request */
	size_used = scnprintf(buffer, size, "load name: %s\nload type: %s\n",
				      load_descs->load_name,
				      load_type[owner_info->type]);

	/* list of configurations loaded by this owner element */
	size_used += scnprintf(buffer + size_used, size - size_used,
			       "(configurations: ");
	if (!(size_used < size))
		goto buffer_done;

	if (!load_descs->config_descs[0]) {
		size_used += scnprintf(buffer + size_used, size - size_used,
				       " None )\n");
		if (!(size_used < size))
			goto buffer_done;
	} else {
		i = 0;
		while (load_descs->config_descs[i] && (size_used < size)) {
			size_used += scnprintf(buffer + size_used,
					       size - size_used, " %s",
					       load_descs->config_descs[i]->name);
			i++;
		}
		size_used +=
			scnprintf(buffer + size_used, size - size_used, " )\n");
	}
	if (!(size_used < size))
		goto buffer_done;

	/* list of features loaded by this owner element */
	size_used += scnprintf(buffer + size_used, size - size_used, "(features: ");
	if (!(size_used < size))
		goto buffer_done;

	if (!load_descs->feat_descs[0]) {
		size_used +=
			scnprintf(buffer + size_used, size - size_used, " None )\n");
		if (!(size_used < size))
			goto buffer_done;
	} else {
		i = 0;
		while (load_descs->feat_descs[i] && (size_used < size)) {
			size_used += scnprintf(buffer + size_used,
					       size - size_used, " %s",
					       load_descs->feat_descs[i]->name);
			i++;
		}
		size_used +=
			scnprintf(buffer + size_used, size - size_used, " )\n");
	}

	/* done or buffer full */
buffer_done:
	return size_used;
}

void cscfg_enable_dyn_load(void)
{
	mutex_lock(&cfs_mutex);
	cscfg_dyn_load_enabled = true;
	mutex_unlock(&cfs_mutex);
}

/* disable dynamic load / unload if no current unload scheduled */
bool cscfg_disable_dyn_load(void)
{
	mutex_lock(&cfs_mutex);
	if (!cscfg_sched_dyn_unload_owner)
		cscfg_dyn_load_enabled = false;
	mutex_unlock(&cfs_mutex);
	return !cscfg_dyn_load_enabled;
}

void cscfg_at_exit_dyn_load(void)
{
	mutex_lock(&cfs_mutex);
	cscfg_dyn_load_enabled = false;
	cscfg_sched_dyn_unload_owner = NULL;
	mutex_unlock(&cfs_mutex);
}


struct cscfg_load_owner_info *cscfg_create_dyn_load_owner_info(void)
{
	struct cscfg_table_load_descs *load_descs = 0;
	struct cscfg_load_owner_info *owner_info = 0;

	load_descs = kzalloc(sizeof(struct cscfg_table_load_descs), GFP_KERNEL);
	if (!load_descs)
		return owner_info;

	owner_info = kzalloc(sizeof(struct cscfg_load_owner_info), GFP_KERNEL);
	if (owner_info) {
		owner_info->owner_handle = load_descs;
		owner_info->type = CSCFG_OWNER_DYNLOAD;
	} else
		kfree(load_descs);

	return owner_info;
}

/* free memory associated with a dynamically loaded configuration & descriptors */
void cscfg_free_dyn_load_owner_info(struct cscfg_load_owner_info *owner_info)
{
	struct cscfg_table_load_descs *load_descs = 0;

	if (!owner_info)
		return;

	if (owner_info->type != CSCFG_OWNER_DYNLOAD)
		return;

	load_descs = (struct cscfg_table_load_descs *)(owner_info->owner_handle);

	if (load_descs) {
		/* free the data allocated on table load, pointed to by load_descs */
		cscfg_table_free_load_descs(load_descs);
		kfree(load_descs);
	}

	kfree(owner_info);
}

/* return load name if dynamic load owned element */
const char *cscfg_get_dyn_load_name(struct cscfg_load_owner_info *owner_info)
{
	const char *name = "unknown";
	struct cscfg_table_load_descs *load_descs;

	if (!owner_info)
		return name;

	load_descs = (struct cscfg_table_load_descs *)(owner_info->owner_handle);
	if (owner_info->type == CSCFG_OWNER_DYNLOAD)
		return load_descs->load_name;

	return name;
}

/*
 * Dynamic load and unload configuration table API
 */

/* dynamically load a configuration and features from a config table
 */
int cscfg_dyn_load_cfg_table(const void *table, size_t table_size)
{
	struct cscfg_table_load_descs *load_descs = 0;
	struct cscfg_load_owner_info *owner_info = 0;
	int err = -EINVAL;

	/* ensure we cannot simultaneously load and unload */
	if (!mutex_trylock(&cfs_mutex)) {
		err = -EBUSY;
		goto exit_unlock;
	}

	/* check configfs load / unload ops are permitted */
	if (!cscfg_load_ops_permitted()) {
		err = -EBUSY;
		goto exit_unlock;
	}

	if (table_size > CSCFG_TABLE_MAXSIZE) {
		pr_err("cscfg: Load error - Input file too large.\n");
		goto exit_unlock;
	}

	/* create owner info as dyn load type with descriptor tables to be filled */
	owner_info = cscfg_create_dyn_load_owner_info();
	if (owner_info)
		load_descs = (struct cscfg_table_load_descs *)(owner_info->owner_handle);
	else {
		err = -ENOMEM;
		goto exit_unlock;
	}

	/* convert table into internal data structures */
	err = cscfg_table_read_buffer(table, table_size, load_descs);
	if (err) {
		pr_err("cscfg: Load error - Failed to read input buffer.\n");
		goto exit_memfree;
	}

	err = cscfg_load_config_sets(load_descs->config_descs, load_descs->feat_descs, owner_info);
	if (err) {
		pr_err("cscfg: Load error - Failed to load configuaration table.\n");
		goto exit_memfree;
	}

	/* load success */
	goto exit_unlock;

exit_memfree:
	/* frees up owner_info and load_descs */
	cscfg_free_dyn_load_owner_info(owner_info);

exit_unlock:
	mutex_unlock(&cfs_mutex);
	return err;
}
EXPORT_SYMBOL_GPL(cscfg_dyn_load_cfg_table);

/*
 * schedule the unload of the last dynamically loaded table.
 * load / unload ordering is strictly enforced.
 */
int cscfg_sched_dyn_unload_cfg_table(void)
{
	struct cscfg_load_owner_info *owner_info = 0;
	int err = -EINVAL;

	/* ensure we cannot simultaneously load and unload */
	if (!mutex_trylock(&cfs_mutex)) {
		err = -EBUSY;
		goto exit_unlock;
	}

	/* check dyn load / unload ops are permitted & no ongoing unload */
	if (!cscfg_load_ops_permitted()) {
		err = -EBUSY;
		goto exit_unlock;
	}

	/* find the last loaded owner info block */
	owner_info = cscfg_find_last_loaded_cfg_owner();
	if (!owner_info) {
		pr_err("cscfg: Unload error: Failed to find any loaded configuration\n");
		goto exit_unlock;
	}

	if (owner_info->type != CSCFG_OWNER_DYNLOAD) {
		pr_err("cscfg: Unload error: Last loaded configuration not dynamic loaded item\n");
		goto exit_unlock;
	}

	/* set cscfg state as starting an unload operation */
	err = cscfg_set_unload_start();
	if (err) {
		pr_err("Config unload %s: failed to set unload start flag\n",
		       cscfg_get_dyn_load_name(owner_info));
		goto exit_unlock;
	}

	/*
	 * actual unload is scheduled as a work item to avoid
	 * lockdep issues when triggered from configfs
	 */
	cscfg_sched_dyn_unload_owner = owner_info;
	err = cscfg_schedule_unload();

exit_unlock:
	mutex_unlock(&cfs_mutex);
	return err;
}
EXPORT_SYMBOL_GPL(cscfg_sched_dyn_unload_cfg_table);

/*
 * configfs object and directory operations
 */

/* create a default ci_type. */
static inline struct config_item_type *cscfg_create_ci_type(void)
{
	struct config_item_type *ci_type;

	ci_type = kzalloc(sizeof(*ci_type), GFP_KERNEL);
	if (ci_type)
		ci_type->ct_owner = THIS_MODULE;

	return ci_type;
}

/* configurations sub-group */

/* attributes for the config view group */
static ssize_t cscfg_cfg_description_show(struct config_item *item, char *page)
{
	struct cscfg_fs_config *fs_config = container_of(to_config_group(item),
							 struct cscfg_fs_config, group);

	return scnprintf(page, PAGE_SIZE, "%s", fs_config->config_desc->description);
}
CONFIGFS_ATTR_RO(cscfg_cfg_, description);

static ssize_t cscfg_cfg_feature_refs_show(struct config_item *item, char *page)
{
	struct cscfg_fs_config *fs_config = container_of(to_config_group(item),
							 struct cscfg_fs_config, group);
	const struct cscfg_config_desc *config_desc = fs_config->config_desc;
	ssize_t ch_used = 0;
	int i;

	for (i = 0; i < config_desc->nr_feat_refs; i++)
		ch_used += scnprintf(page + ch_used, PAGE_SIZE - ch_used,
				     "%s\n", config_desc->feat_ref_names[i]);
	return ch_used;
}
CONFIGFS_ATTR_RO(cscfg_cfg_, feature_refs);

/* list preset values in order of features and params */
static ssize_t cscfg_cfg_values_show(struct config_item *item, char *page)
{
	const struct cscfg_feature_desc *feat_desc;
	const struct cscfg_config_desc *config_desc;
	struct cscfg_fs_preset *fs_preset;
	int i, j, val_idx, preset_idx;
	ssize_t used = 0;

	fs_preset = container_of(to_config_group(item), struct cscfg_fs_preset, group);
	config_desc = fs_preset->config_desc;

	if (!config_desc->nr_presets)
		return 0;

	preset_idx = fs_preset->preset_num - 1;

	/* start index on the correct array line */
	val_idx = config_desc->nr_total_params * preset_idx;

	/*
	 * A set of presets is the sum of all params in used features,
	 * in order of declaration of features and params in the features
	 */
	for (i = 0; i < config_desc->nr_feat_refs; i++) {
		feat_desc = cscfg_get_named_feat_desc(config_desc->feat_ref_names[i]);
		for (j = 0; j < feat_desc->nr_params; j++) {
			used += scnprintf(page + used, PAGE_SIZE - used,
					  "%s.%s = 0x%llx ",
					  feat_desc->name,
					  feat_desc->params_desc[j].name,
					  config_desc->presets[val_idx++]);
		}
	}
	used += scnprintf(page + used, PAGE_SIZE - used, "\n");

	return used;
}
CONFIGFS_ATTR_RO(cscfg_cfg_, values);

static ssize_t cscfg_cfg_enable_show(struct config_item *item, char *page)
{
	struct cscfg_fs_config *fs_config = container_of(to_config_group(item),
							 struct cscfg_fs_config, group);

	return scnprintf(page, PAGE_SIZE, "%d\n", fs_config->active);
}

static ssize_t cscfg_cfg_enable_store(struct config_item *item,
					const char *page, size_t count)
{
	struct cscfg_fs_config *fs_config = container_of(to_config_group(item),
							 struct cscfg_fs_config, group);
	int err;
	bool val;

	err = kstrtobool(page, &val);
	if (!err)
		err = cscfg_config_sysfs_activate(fs_config->config_desc, val);
	if (!err) {
		fs_config->active = val;
		if (val)
			cscfg_config_sysfs_set_preset(fs_config->preset);
	}
	return err ? err : count;
}
CONFIGFS_ATTR(cscfg_cfg_, enable);

static ssize_t cscfg_cfg_preset_show(struct config_item *item, char *page)
{
	struct cscfg_fs_config *fs_config = container_of(to_config_group(item),
							 struct cscfg_fs_config, group);

	return scnprintf(page, PAGE_SIZE, "%d\n", fs_config->preset);
}

static ssize_t cscfg_cfg_preset_store(struct config_item *item,
					     const char *page, size_t count)
{
	struct cscfg_fs_config *fs_config = container_of(to_config_group(item),
							 struct cscfg_fs_config, group);
	int preset, err;

	err = kstrtoint(page, 0, &preset);
	if (!err) {
		/*
		 * presets start at 1, and go up to max (15),
		 * but the config may provide fewer.
		 */
		if ((preset < 1) || (preset > fs_config->config_desc->nr_presets))
			err = -EINVAL;
	}

	if (!err) {
		/* set new value */
		fs_config->preset = preset;
		/* set on system if active */
		if (fs_config->active)
			cscfg_config_sysfs_set_preset(fs_config->preset);
	}
	return err ? err : count;
}
CONFIGFS_ATTR(cscfg_cfg_, preset);

static struct configfs_attribute *cscfg_config_view_attrs[] = {
	&cscfg_cfg_attr_description,
	&cscfg_cfg_attr_feature_refs,
	&cscfg_cfg_attr_enable,
	&cscfg_cfg_attr_preset,
	NULL,
};

static struct config_item_type cscfg_config_view_type = {
	.ct_owner = THIS_MODULE,
	.ct_attrs = cscfg_config_view_attrs,
};

static struct configfs_attribute *cscfg_config_preset_attrs[] = {
	&cscfg_cfg_attr_values,
	NULL,
};

static struct config_item_type cscfg_config_preset_type = {
	.ct_owner = THIS_MODULE,
	.ct_attrs = cscfg_config_preset_attrs,
};


/* walk list of presets and free the previously allocated memory */
static void cscfg_destroy_preset_groups(struct config_group *cfg_view_group)
{
	struct cscfg_fs_preset *cfg_fs_preset;
	struct config_group *p_group;

	list_for_each_entry(p_group, &cfg_view_group->default_groups, default_groups) {
		cfg_fs_preset = container_of(p_group, struct cscfg_fs_preset, group);
		kfree(cfg_fs_preset);
	}
}

static int cscfg_add_preset_groups(struct cscfg_fs_config *cfg_view)
{
	int preset_num;
	struct cscfg_fs_preset *cfg_fs_preset;
	struct cscfg_config_desc *config_desc = cfg_view->config_desc;
	char name[CONFIGFS_ITEM_NAME_LEN];

	if (!config_desc->nr_presets)
		return 0;

	for (preset_num = 1; preset_num <= config_desc->nr_presets; preset_num++) {
		cfg_fs_preset = kzalloc(sizeof(struct cscfg_fs_preset), GFP_KERNEL);

		if (!cfg_fs_preset) {
			cscfg_destroy_preset_groups(&cfg_view->group);
			return -ENOMEM;
		}

		snprintf(name, CONFIGFS_ITEM_NAME_LEN, "preset%d", preset_num);
		cfg_fs_preset->preset_num = preset_num;
		cfg_fs_preset->config_desc = cfg_view->config_desc;
		config_group_init_type_name(&cfg_fs_preset->group, name,
					    &cscfg_config_preset_type);
		configfs_add_default_group(&cfg_fs_preset->group, &cfg_view->group);
	}
	return 0;
}

static struct config_group *cscfg_create_config_group(struct cscfg_config_desc *config_desc)
{
	struct cscfg_fs_config *cfg_view = NULL;
	int err;

	cfg_view = kzalloc(sizeof(struct cscfg_fs_config), GFP_KERNEL);
	if (!cfg_view)
		return ERR_PTR(-ENOMEM);

	cfg_view->config_desc = config_desc;
	config_group_init_type_name(&cfg_view->group, config_desc->name, &cscfg_config_view_type);

	/* add in a preset<n> dir for each preset */
	err = cscfg_add_preset_groups(cfg_view);
	if (err) {
		kfree(cfg_view);
		return ERR_PTR(err);
	}
	return &cfg_view->group;
}

static void cscfg_destroy_config_group(struct config_group *group)
{
	struct cscfg_fs_config *cfg_view = container_of(group, struct cscfg_fs_config, group);

	cscfg_destroy_preset_groups(&cfg_view->group);
	kfree(cfg_view);
}

/* attributes for features view */

static ssize_t cscfg_feat_description_show(struct config_item *item, char *page)
{
	struct cscfg_fs_feature *fs_feat = container_of(to_config_group(item),
							struct cscfg_fs_feature, group);

	return scnprintf(page, PAGE_SIZE, "%s", fs_feat->feat_desc->description);
}
CONFIGFS_ATTR_RO(cscfg_feat_, description);

static ssize_t cscfg_feat_matches_show(struct config_item *item, char *page)
{
	struct cscfg_fs_feature *fs_feat = container_of(to_config_group(item),
							struct cscfg_fs_feature, group);
	u32 match_flags = fs_feat->feat_desc->match_flags;
	int used = 0;

	if (match_flags & CS_CFG_MATCH_CLASS_SRC_ALL)
		used = scnprintf(page, PAGE_SIZE, "SRC_ALL ");

	if (match_flags & CS_CFG_MATCH_CLASS_SRC_ETM4)
		used += scnprintf(page + used, PAGE_SIZE - used, "SRC_ETMV4 ");

	used += scnprintf(page + used, PAGE_SIZE - used, "\n");
	return used;
}
CONFIGFS_ATTR_RO(cscfg_feat_, matches);

static ssize_t cscfg_feat_nr_params_show(struct config_item *item, char *page)
{
	struct cscfg_fs_feature *fs_feat = container_of(to_config_group(item),
							struct cscfg_fs_feature, group);

	return scnprintf(page, PAGE_SIZE, "%d\n", fs_feat->feat_desc->nr_params);
}
CONFIGFS_ATTR_RO(cscfg_feat_, nr_params);

/* base feature desc attrib structures */
static struct configfs_attribute *cscfg_feature_view_attrs[] = {
	&cscfg_feat_attr_description,
	&cscfg_feat_attr_matches,
	&cscfg_feat_attr_nr_params,
	NULL,
};

static struct config_item_type cscfg_feature_view_type = {
	.ct_owner = THIS_MODULE,
	.ct_attrs = cscfg_feature_view_attrs,
};

static ssize_t cscfg_param_value_show(struct config_item *item, char *page)
{
	struct cscfg_fs_param *param_item = container_of(to_config_group(item),
							 struct cscfg_fs_param, group);
	u64 value = param_item->feat_desc->params_desc[param_item->param_idx].value;

	return scnprintf(page, PAGE_SIZE, "0x%llx\n", value);
}

static ssize_t cscfg_param_value_store(struct config_item *item,
				       const char *page, size_t size)
{
	struct cscfg_fs_param *param_item = container_of(to_config_group(item),
							 struct cscfg_fs_param, group);
	struct cscfg_feature_desc *feat_desc = param_item->feat_desc;
	int param_idx = param_item->param_idx;
	u64 value;
	int err;

	err = kstrtoull(page, 0, &value);
	if (!err)
		err = cscfg_update_feat_param_val(feat_desc, param_idx, value);

	return err ? err : size;
}
CONFIGFS_ATTR(cscfg_param_, value);

static struct configfs_attribute *cscfg_param_view_attrs[] = {
	&cscfg_param_attr_value,
	NULL,
};

static struct config_item_type cscfg_param_view_type = {
	.ct_owner = THIS_MODULE,
	.ct_attrs = cscfg_param_view_attrs,
};

/* walk the list of default groups - which were set as param items and remove */
static void cscfg_destroy_params_group_items(struct config_group *params_group)
{
	struct cscfg_fs_param *param_item;
	struct config_group *p_group;

	list_for_each_entry(p_group, &params_group->default_groups, default_groups) {
		param_item = container_of(p_group, struct cscfg_fs_param, group);
		kfree(param_item);
	}
}
/*
 * configfs has far less functionality provided to add attributes dynamically than sysfs,
 * and the show and store fns pass the enclosing config_item so the actual attribute cannot
 * be determined. Therefore we add each item as a group directory, with a value attribute.
 */
static int cscfg_create_params_group_items(struct cscfg_feature_desc *feat_desc,
					   struct config_group *params_group)
{
	struct cscfg_fs_param *param_item;
	int i;

	/* parameter items - as groups with default_value attribute */
	for (i = 0; i < feat_desc->nr_params; i++) {
		param_item = kzalloc(sizeof(struct cscfg_fs_param), GFP_KERNEL);
		if (!param_item) {
			cscfg_destroy_params_group_items(params_group);
			return -ENOMEM;
		}
		param_item->feat_desc = feat_desc;
		param_item->param_idx = i;
		config_group_init_type_name(&param_item->group,
					    feat_desc->params_desc[i].name,
					    &cscfg_param_view_type);
		configfs_add_default_group(&param_item->group, params_group);
	}
	return 0;
}

static struct config_group *cscfg_create_feature_group(struct cscfg_feature_desc *feat_desc)
{
	struct cscfg_fs_feature *feat_view = NULL;
	struct config_item_type *params_group_type = NULL;
	struct config_group *params_group = NULL;
	int err = -ENOMEM;

	feat_view = kzalloc(sizeof(struct cscfg_fs_feature), GFP_KERNEL);
	if (!feat_view)
		return ERR_PTR(-ENOMEM);

	if (feat_desc->nr_params) {
		params_group = kzalloc(sizeof(struct config_group), GFP_KERNEL);
		if (!params_group)
			goto exit_err_free_mem;
		params_group_type = cscfg_create_ci_type();
		if (!params_group_type)
			goto exit_err_free_mem;
	}

	feat_view->feat_desc = feat_desc;
	config_group_init_type_name(&feat_view->group,
				    feat_desc->name,
				    &cscfg_feature_view_type);
	if (params_group) {
		config_group_init_type_name(params_group, "params", params_group_type);
		configfs_add_default_group(params_group, &feat_view->group);
		err = cscfg_create_params_group_items(feat_desc, params_group);
		if (err)
			goto exit_err_free_mem;
	}
	return &feat_view->group;

exit_err_free_mem:
	kfree(feat_view);
	kfree(params_group_type);
	kfree(params_group);
	return ERR_PTR(err);
}

static void cscfg_destroy_feature_group(struct config_group *feat_group)
{
	struct cscfg_fs_feature *feat_view;
	struct config_group *params_group = NULL;

	feat_view = container_of(feat_group, struct cscfg_fs_feature, group);

	/* params group is the first item on the default group list */
	if (!list_empty(&feat_group->default_groups)) {
		params_group = list_first_entry(&feat_group->default_groups,
						struct config_group, default_groups);
		cscfg_destroy_params_group_items(params_group);
		/* free the item type, then the group */
		kfree(params_group->cg_item.ci_type);
		kfree(params_group);
	}
	kfree(feat_view);
}


static struct config_item_type cscfg_configs_load_type = {
	.ct_owner = THIS_MODULE,
};

/* configurations group */
static struct config_item_type cscfg_configs_grp_type = {
	.ct_owner = THIS_MODULE,
};

static struct config_group cscfg_configs_grp = {
	.cg_item = {
		.ci_namebuf = "configurations",
		.ci_type = &cscfg_configs_grp_type,
	},
};

/* add configuration to configurations group */
int cscfg_configfs_add_config(struct cscfg_config_desc *config_desc)
{
	struct config_group *new_group;
	int err;

	new_group = cscfg_create_config_group(config_desc);
	if (IS_ERR(new_group))
		return PTR_ERR(new_group);
	err =  configfs_register_group(&cscfg_configs_grp, new_group);
	if (!err)
		config_desc->fs_group = new_group;
	else
		cscfg_destroy_config_group(new_group);
	return err;
}

void cscfg_configfs_del_config(struct cscfg_config_desc *config_desc)
{
	if (config_desc->fs_group) {
		configfs_unregister_group(config_desc->fs_group);
		cscfg_destroy_config_group(config_desc->fs_group);
		config_desc->fs_group = NULL;
	}
}

static struct config_item_type cscfg_features_type = {
	.ct_owner = THIS_MODULE,
};

static struct config_group cscfg_features_grp = {
	.cg_item = {
		.ci_namebuf = "features",
		.ci_type = &cscfg_features_type,
	},
};

/* add feature to features group */
int cscfg_configfs_add_feature(struct cscfg_feature_desc *feat_desc)
{
	struct config_group *new_group;
	int err;

	new_group = cscfg_create_feature_group(feat_desc);
	if (IS_ERR(new_group))
		return PTR_ERR(new_group);
	err = configfs_register_group(&cscfg_features_grp, new_group);
	if (!err)
		feat_desc->fs_group = new_group;
	else
		cscfg_destroy_feature_group(new_group);
	return err;
}

void cscfg_configfs_del_feature(struct cscfg_feature_desc *feat_desc)
{
	if (feat_desc->fs_group) {
		configfs_unregister_group(feat_desc->fs_group);
		cscfg_destroy_feature_group(feat_desc->fs_group);
		feat_desc->fs_group = NULL;
	}
}

int cscfg_configfs_init(struct cscfg_manager *cscfg_mgr)
{
	struct configfs_subsystem *subsys;

	if (!cscfg_mgr)
		return -EINVAL;

	/* dyncamic load and unload initially disabled */
	cscfg_dyn_load_enabled = false;

	/* no current scheduled unload operation in progress */
	cscfg_sched_dyn_unload_owner = NULL;

	subsys = &cscfg_mgr->cfgfs_subsys;
	config_item_set_name(&subsys->su_group.cg_item, CSCFG_FS_SUBSYS_NAME);
	subsys->su_group.cg_item.ci_type = &cscfg_configs_load_type;

	config_group_init(&subsys->su_group);
	mutex_init(&subsys->su_mutex);

	/* Add default groups to subsystem */
	config_group_init(&cscfg_configs_grp);
	configfs_add_default_group(&cscfg_configs_grp, &subsys->su_group);

	config_group_init(&cscfg_features_grp);
	configfs_add_default_group(&cscfg_features_grp, &subsys->su_group);

	return configfs_register_subsystem(subsys);
}

void cscfg_configfs_release(struct cscfg_manager *cscfg_mgr)
{
	configfs_unregister_subsystem(&cscfg_mgr->cfgfs_subsys);
}
