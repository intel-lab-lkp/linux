// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2018-2026, Advanced Micro Devices, Inc. */

#include <linux/debugfs.h>

#include "ionic_ibdev.h"
#include "ionic_profiles.h"

static const struct ionic_profile_vals dcqcn_defaults[] = {
	{
		.v[NP_ICNP_802P_PRIO]			= 6,
		.v[NP_CNP_DSCP]				= 46,
		.v[RP_TOKEN_BUCKET_SIZE]		= 800000,
		.v[RP_INITIAL_ALPHA_VALUE]		= 64,
		.v[RP_DCE_TCP_G]			= 512,
		.v[RP_DCE_TCP_RTT]			= 1,
		.v[RP_RATE_REDUCE_MONITOR_PERIOD]	= 1,
		.v[RP_MIN_RATE]				= 1,
		.v[RP_GD]				= 11,
		.v[RP_MIN_DEC_FAC]			= 50,
		.v[RP_CLAMP_TGT_RATE_ATI]		= 1,
		.v[RP_THRESHOLD]			= 1,
		.v[RP_TIME_RESET]			= 1,
		.v[RP_QP_RATE]				= 100000,
		.v[RP_BYTE_RESET]			= 431068,
		.v[RP_AI_RATE]				= 160,
		.v[RP_HAI_RATE]				= 300,
	},
};

#define DCQCN_INT_ATTR(_min, _max, _name) \
	{ .min = (_min), .max = (_max), .name = (_name) }

#define DCQCN_BOOL_ATTR(_name) \
	DCQCN_INT_ATTR(0, 1, _name)

static const struct ionic_dcqcn_param_attr dcqcn_attrs[] = {
	/* under "roce_np" */
	DCQCN_INT_ATTR(0, 7, "icnp_802p_prio"),
	DCQCN_INT_ATTR(0, 63, "cnp_dscp"),
	/* under "roce_rp" */
	DCQCN_INT_ATTR(100, 200000000, "token_bucket_size"),
	DCQCN_INT_ATTR(0, 1023, "initial_alpha_value"),
	DCQCN_INT_ATTR(0, 1023, "dce_tcp_g"),
	DCQCN_INT_ATTR(1, 131071, "dce_tcp_rtt"),
	DCQCN_INT_ATTR(1, INT_MAX, "rate_reduce_monitor_period"),
	DCQCN_INT_ATTR(1, INT_MAX, "rate_to_set_on_first_cnp"),
	DCQCN_INT_ATTR(1, INT_MAX, "min_rate"),
	DCQCN_INT_ATTR(1, 11, "gd"),
	DCQCN_INT_ATTR(0, 100, "min_dec_fac"),
	DCQCN_BOOL_ATTR("clamp_tgt_rate"),
	DCQCN_BOOL_ATTR("clamp_tgt_rate_ati"),
	DCQCN_INT_ATTR(1, 31, "threshold"),
	DCQCN_INT_ATTR(1, 32767, "time_reset"),
	DCQCN_INT_ATTR(1, INT_MAX, "qp_rate"),
	DCQCN_INT_ATTR(1, INT_MAX, "byte_reset"),
	DCQCN_INT_ATTR(1, INT_MAX, "ai_rate"),
	DCQCN_INT_ATTR(1, INT_MAX, "hai_rate"),
};

static void dcqcn_set_profile(struct ionic_profile *profile)
{
	struct ionic_ibdev *dev = profile->dev;
	struct ionic_admin_wr wr = {
		.work = COMPLETION_INITIALIZER_ONSTACK(wr.work),
		.wqe = {
			.op = IONIC_V1_ADMIN_MODIFY_DCQCN,
			.len = cpu_to_le16(IONIC_ADMIN_MODIFY_DCQCN_IN_V1_LEN),
			.cmd.mod_dcqcn = {
				.id_ver = cpu_to_le32(profile->idx + 1),
			}
		}
	};
	int rc;

	wr.wqe.cmd.mod_dcqcn.np_incp_802p_prio =
		profile->vals.v[NP_ICNP_802P_PRIO];

	wr.wqe.cmd.mod_dcqcn.np_cnp_dscp =
		profile->vals.v[NP_CNP_DSCP];

	wr.wqe.cmd.mod_dcqcn.rp_token_bucket_size =
		cpu_to_be64(profile->vals.v[RP_TOKEN_BUCKET_SIZE]);

	wr.wqe.cmd.mod_dcqcn.rp_initial_alpha_value =
		cpu_to_be16(profile->vals.v[RP_INITIAL_ALPHA_VALUE]);

	wr.wqe.cmd.mod_dcqcn.rp_dce_tcp_g =
		cpu_to_be16(profile->vals.v[RP_DCE_TCP_G]);

	wr.wqe.cmd.mod_dcqcn.rp_dce_tcp_rtt =
		cpu_to_be32(profile->vals.v[RP_DCE_TCP_RTT]);

	wr.wqe.cmd.mod_dcqcn.rp_rate_reduce_monitor_period =
		cpu_to_be32(profile->vals.v[RP_RATE_REDUCE_MONITOR_PERIOD]);

	wr.wqe.cmd.mod_dcqcn.rp_rate_to_set_on_first_cnp =
		cpu_to_be32(profile->vals.v[RP_RATE_TO_SET_ON_FIRST_CNP]);

	wr.wqe.cmd.mod_dcqcn.rp_min_rate =
		cpu_to_be32(profile->vals.v[RP_MIN_RATE]);

	wr.wqe.cmd.mod_dcqcn.rp_gd =
		profile->vals.v[RP_GD];

	wr.wqe.cmd.mod_dcqcn.rp_min_dec_fac =
		profile->vals.v[RP_MIN_DEC_FAC];

	if (profile->vals.v[RP_CLAMP_TGT_RATE])
		wr.wqe.cmd.mod_dcqcn.rp_clamp_flags |= IONIC_RPF_CLAMP_TGT_RATE;

	if (profile->vals.v[RP_CLAMP_TGT_RATE_ATI])
		wr.wqe.cmd.mod_dcqcn.rp_clamp_flags |=
			IONIC_RPF_CLAMP_TGT_RATE_ATI;

	wr.wqe.cmd.mod_dcqcn.rp_threshold =
		profile->vals.v[RP_THRESHOLD];

	wr.wqe.cmd.mod_dcqcn.rp_time_reset =
		cpu_to_be32(profile->vals.v[RP_TIME_RESET]);

	wr.wqe.cmd.mod_dcqcn.rp_qp_rate =
		cpu_to_be32(profile->vals.v[RP_QP_RATE]);

	wr.wqe.cmd.mod_dcqcn.rp_byte_reset =
		cpu_to_be32(profile->vals.v[RP_BYTE_RESET]);

	wr.wqe.cmd.mod_dcqcn.rp_ai_rate =
		cpu_to_be32(profile->vals.v[RP_AI_RATE]);

	wr.wqe.cmd.mod_dcqcn.rp_hai_rate =
		cpu_to_be32(profile->vals.v[RP_HAI_RATE]);

	ionic_admin_post(dev, &wr);
	rc = ionic_admin_wait(dev, &wr, IONIC_ADMIN_F_INTERRUPT);
	if (rc)
		ibdev_warn(&dev->ibdev, "dcqcn profile %d not set, error %d\n",
			   profile->idx + 1, rc);
}

static int dcqcn_param_show(struct seq_file *s, void *v)
{
	struct ionic_dcqcn_param_entry *entry = s->private;
	struct ionic_profile *profile = entry->profile;
	int val = profile->vals.v[entry->var];

	seq_printf(s, "%d\n", val);
	return 0;
}

static ssize_t dcqcn_param_write(struct file *fp, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	struct seq_file *s = fp->private_data;
	struct ionic_dcqcn_param_entry *entry;
	struct ionic_profile *profile;
	int rc, val;
	char *buf;

	entry = s->private;
	profile = entry->profile;

	buf = memdup_user_nul(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	rc = kstrtoint(buf, 0, &val);
	if (rc < 0)
		goto out;

	if (val < dcqcn_attrs[entry->var].min ||
	    val > dcqcn_attrs[entry->var].max) {
		rc = -EINVAL;
		goto out;
	}

	profile->vals.v[entry->var] = val;

	dcqcn_set_profile(profile);
out:
	kfree(buf);
	return rc ?: count;
}
DEFINE_SHOW_STORE_ATTRIBUTE(dcqcn_param);

static const struct ionic_profile_vals *dcqcn_get_defaults(int prof_i)
{
	if (prof_i < 0 || prof_i >= ARRAY_SIZE(dcqcn_defaults))
		return &dcqcn_defaults[0];

	return &dcqcn_defaults[prof_i];
}

static int dcqcn_match_default_show(struct seq_file *s, void *v)
{
	struct ionic_profile_root *profile_root = s->private;
	int val = profile_root->profiles_default;

	seq_printf(s, "%d\n", val);
	return 0;
}

static ssize_t dcqcn_match_default_write(struct file *fp, const char __user *ubuf,
					 size_t count, loff_t *ppos)
{
	struct ionic_profile_root *profile_root;
	struct seq_file *s = fp->private_data;
	int rc, val;
	char *buf;

	profile_root = s->private;

	buf = memdup_user_nul(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	rc = kstrtoint(buf, 0, &val);
	if (rc < 0)
		goto out;

	if (val < 0 || val > profile_root->profiles_count) {
		rc = -EINVAL;
		goto out;
	}

	profile_root->profiles_default = val;

out:
	kfree(buf);
	return rc ?: count;
}
DEFINE_SHOW_STORE_ATTRIBUTE(dcqcn_match_default);

static int dcqcn_match_rules_show(struct seq_file *s, void *v)
{
	struct ionic_profile_root *profile_root = s->private;
	struct ionic_match_rule *rule, *rules;
	unsigned long irqflags;
	int i, rules_count;

	spin_lock_irqsave(&profile_root->rules_lock, irqflags);

	rules = profile_root->rules;
	rules_count = profile_root->rules_count;
	for (i = 0; i < rules_count; ++i) {
		rule = &rules[i];
		seq_printf(s, "%s %d %d\n", rule->name, rule->cond, rule->prof);
	}

	spin_unlock_irqrestore(&profile_root->rules_lock, irqflags);

	return 0;
}

static bool ionic_match_prio(struct rdma_ah_attr *attr, int cond)
{
	int prio = attr->sl;

	return prio >= 0 && prio < 8 && (cond & BIT(prio));
}

static bool ionic_match_gid(struct rdma_ah_attr *attr, int cond)
{
	int gid = rdma_ah_read_grh(attr)->sgid_index;

	return gid == cond;
}

static bool ionic_parse_rule_name(const char *name, const char *buf, int count)
{
	return !strncmp(name, buf, count) && !name[count];
}

static int ionic_parse_match_rules(const char *buf, size_t count,
				   int prof_count, int rules_count,
				   struct ionic_match_rule *rules)
{
	bool (*match)(struct rdma_ah_attr *attr, int cond);
	struct ionic_match_rule *rule;
	int cmd, cond, prof, end;
	int rc, rule_i = 0;
	const char *name;

	for (;; ++rule_i) {
		/* skip leading whitespace */

		rc = sscanf(buf, " %n", &end);
		if (rc != 0)
			return -EINVAL;

		buf += end;
		count -= end;

		/* break at end of buffer */

		if (!count)
			break;

		/* Parse one rule, as:
		 * <name> <condition> <profile>
		 *
		 * Name and condition determine when a rule will be a match.
		 * If a rule is a match, then use the inidcated DCQCN profile.
		 *
		 * If name eq "gid":
		 * then condition is a gid index.
		 *
		 * eg: gid 5 3 -> for gid index 5, use profile 3.
		 *
		 * If name eq "prio":
		 * then condition is a bitmask of 802.1p priorities.
		 *
		 * eg: prio 0xc 1 -> for 802.1p priority 2 or 3, use profile 1.
		 */

		rc = sscanf(buf, "%*s%n%i%i%n", &cmd, &cond, &prof, &end);
		if (rc != 2)
			return -EINVAL;

		/* rule name in first `cmd` chars of `buf` */

		if (ionic_parse_rule_name("gid", buf, cmd)) {
			match = ionic_match_gid;
			name = "gid";
		} else if (ionic_parse_rule_name("prio", buf, cmd)) {
			match = ionic_match_prio;
			name = "prio";
		} else {
			return -EINVAL;
		}

		if (prof < 0 || prof > prof_count)
			return -EINVAL;

		if (rule_i < rules_count) {
			rule = &rules[rule_i];
			rule->match = match;
			rule->name = name;
			rule->cond = cond;
			rule->prof = prof;
		}

		buf += end;
		count -= end;
	}

	return rule_i;
}

static ssize_t dcqcn_match_rules_write(struct file *fp,
				       const char __user *ubuf,
				       size_t count, loff_t *ppos)
{
	struct ionic_profile_root *profile_root;
	struct seq_file *s = fp->private_data;
	unsigned long irqflags;
	int rc, rules_count;
	char *buf;

	profile_root = s->private;

	buf = memdup_user_nul(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	/* validate and count rules */
	rc = ionic_parse_match_rules(buf, count, profile_root->profiles_count,
				     0, NULL);
	if (rc < 0)
		goto out;

	rules_count = rc;
	rc = 0;

	/* clear previous rules */
	spin_lock_irqsave(&profile_root->rules_lock, irqflags);
	profile_root->rules_count = 0;
	spin_unlock_irqrestore(&profile_root->rules_lock, irqflags);

	kfree(profile_root->rules);
	profile_root->rules = NULL;

	/* assign new rules */
	if (rules_count) {
		profile_root->rules = kzalloc_objs(*profile_root->rules,
						   rules_count);
		if (!profile_root->rules) {
			rc = -ENOMEM;
			goto out;
		}

		ionic_parse_match_rules(buf, count, profile_root->profiles_count,
					rules_count, profile_root->rules);

		spin_lock_irqsave(&profile_root->rules_lock, irqflags);
		profile_root->rules_count = rules_count;
		spin_unlock_irqrestore(&profile_root->rules_lock, irqflags);
	}
out:
	kfree(buf);
	return rc ?: count;
}
DEFINE_SHOW_STORE_ATTRIBUTE(dcqcn_match_rules);

static ssize_t dcqcn_profile_reset(struct file *fp, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct ionic_profile *profile = fp->private_data;
	int rc = 0;
	char *buf;

	buf = memdup_user_nul(ubuf, 2);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	if (strcmp(buf, "1") && strcmp(buf, "1\n")) {
		rc = -EINVAL;
		goto out;
	}

	profile->vals = *dcqcn_get_defaults(profile->idx);
	dcqcn_set_profile(profile);

out:
	kfree(buf);
	return rc ?: count;
}

static const struct file_operations dcqcn_profile_reset_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = dcqcn_profile_reset,
};

static void ionic_dcqcn_add_profile_params(struct ionic_profile *profile)
{
	struct ionic_dcqcn_param_entry *entry;
	struct dentry *dentry;
	int i;

	for (i = 0; i < DCQCN_VAR_COUNT; i++) {
		entry = &profile->entries[i];

		entry->profile = profile;
		entry->var = i;

		if (i <= NP_CNP_DSCP)
			dentry = profile->roce_np_debug;
		else
			dentry = profile->roce_rp_debug;

		debugfs_create_file(dcqcn_attrs[i].name, 0640, dentry, entry,
				    &dcqcn_param_fops);
	}
}

int ionic_dcqcn_init(struct ionic_ibdev *dev, int prof_count)
{
	const enum ionic_profile_type type = IONIC_PROFILE_TYPE_DCQCN;
	struct ionic_profile_root *profile_root;
	int rc, i;

	if (!prof_count)
		return 0;

	dev->profile[type] = kzalloc_obj(*dev->profile[type]);
	if (!dev->profile[type]) {
		rc = -ENOMEM;
		goto err_cb_alloc;
	}

	profile_root = dev->profile[type];
	profile_root->dev = dev;

	spin_lock_init(&profile_root->rules_lock);

	profile_root->debug = debugfs_create_dir("dcqcn", dev->debug);
	if (IS_ERR(profile_root->debug)) {
		profile_root->debug = NULL;
		goto err_cb_dentry;
	}

	debugfs_create_file("match_default", 0640, profile_root->debug,
			    profile_root, &dcqcn_match_default_fops);
	debugfs_create_file("match_rules", 0640, profile_root->debug,
			    profile_root, &dcqcn_match_rules_fops);

	profile_root->profiles_debug = debugfs_create_dir("profiles",
							  profile_root->debug);
	if (IS_ERR(profile_root->profiles_debug)) {
		profile_root->profiles_debug = NULL;
		goto err_prof_dentry;
	}

	profile_root->profiles = kzalloc_objs(*profile_root->profiles,
					      prof_count);
	if (!profile_root->profiles) {
		rc = -ENOMEM;
		goto err_prof_alloc;
	}

	profile_root->profiles_default = 0;

	for (i = 0; i < prof_count; ++i) {
		struct ionic_profile *profile = &profile_root->profiles[i];
		char name[8];

		profile->dev = dev;
		profile->vals = *dcqcn_get_defaults(i);
		profile->idx = i;

		dcqcn_set_profile(profile);

		snprintf(name, sizeof(name), "%d", i + 1);
		profile->debug = debugfs_create_dir(name,
						 profile_root->profiles_debug);
		if (IS_ERR(profile->debug)) {
			profile->debug = NULL;
			break;
		}

		debugfs_create_file("reset", 0640, profile->debug, profile,
				    &dcqcn_profile_reset_fops);

		profile->entries = kzalloc_objs(*profile->entries,
						DCQCN_VAR_COUNT);

		profile->roce_np_debug = debugfs_create_dir("roce_np",
							    profile->debug);
		if (IS_ERR(profile->roce_np_debug)) {
			profile->roce_np_debug = NULL;
			break;
		}

		profile->roce_rp_debug = debugfs_create_dir("roce_rp",
							    profile->debug);
		if (IS_ERR(profile->roce_rp_debug)) {
			profile->roce_rp_debug = NULL;
			break;
		}

		ionic_dcqcn_add_profile_params(profile);
	}

	if (!i)
		goto err_prof_init;

	profile_root->profiles_count = i;
	if (i != prof_count) {
		ibdev_warn(&dev->ibdev,
			   "dcqcn initialized %d out of %d profiles\n",
			   i, prof_count);
	}
	return 0;

err_prof_init:
	kfree(profile_root->profiles);
err_prof_alloc:
	debugfs_remove_recursive(profile_root->profiles_debug);
err_prof_dentry:
	debugfs_remove_recursive(profile_root->debug);
err_cb_dentry:
	kfree(dev->profile[type]);
	dev->profile[type] = NULL;
err_cb_alloc:
	ibdev_warn(&dev->ibdev, "dcqcn failed init, error %d\n", rc);
	return rc;
}

void ionic_dcqcn_destroy(struct ionic_ibdev *dev)
{
	struct ionic_profile_root *profile_root;
	int i, prof_count;

	profile_root = dev->profile[IONIC_PROFILE_TYPE_DCQCN];

	if (!profile_root)
		return;
	prof_count = profile_root->profiles_count;
	for (i = 0; i < prof_count; ++i) {
		struct ionic_profile *profile;

		profile = &profile_root->profiles[i];

		kfree(profile->entries);
		debugfs_remove_recursive(profile->debug);
	}

	debugfs_remove_recursive(profile_root->profiles_debug);
	kfree(profile_root->rules);
	kfree(profile_root->profiles);

	debugfs_remove_recursive(profile_root->debug);

	kfree(profile_root);
	dev->profile[IONIC_PROFILE_TYPE_DCQCN] = NULL;
}

int ionic_dcqcn_select_profile(struct ionic_ibdev *dev,
			       struct rdma_ah_attr *attr)
{
	struct ionic_profile_root *profile_root;
	struct ionic_match_rule *rule, *rules;
	int i, rules_count, prof;
	unsigned long irqflags;

	if (!dev->profile[IONIC_PROFILE_TYPE_DCQCN])
		return 0;

	profile_root = dev->profile[IONIC_PROFILE_TYPE_DCQCN];

	spin_lock_irqsave(&profile_root->rules_lock, irqflags);

	prof = profile_root->profiles_default;
	rules = profile_root->rules;
	rules_count = profile_root->rules_count;

	for (i = 0; i < rules_count; ++i) {
		rule = &rules[i];
		if (rule->match(attr, rule->cond)) {
			prof = rule->prof;
			break;
		}
	}

	spin_unlock_irqrestore(&profile_root->rules_lock, irqflags);

	return prof;
}
