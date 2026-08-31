// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 IBM Corporation <ssrish@linux.ibm.com>
 *
 * This code exposes wrapping key management options to the user via the
 * sysfs
 */

#define pr_fmt(fmt) "wrapkey-sysfs: " fmt
#define PLPKS_WRAPKEY_REVOKED 1
#define PLPKS_WRAPKEY_UNREVOKED 0

#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/string.h>
#include <linux/of.h>
#include <linux/ctype.h>
#include <linux/unaligned.h>
#include <asm/plpks.h>

static struct kobject *wrapkey_kobj;

static bool is_wrapkey_label_alnum(const char *key_label, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (!isalnum((unsigned char)key_label[i])) {
			pr_err("key label <%*pE> is not alphanumeric\n",
			       (int)len, key_label);
			return false;
		}
	}

	return true;
}

static ssize_t list_key_labels(char *buf, u8 *obj_labels_buf,
			       u64 obj_labels_count, int revoked)
{
	struct plpks_var var = {0};
	u8 *obj_labels_buf_ptr, *comp_prefix;
	u8 *key_label_ptr;
	u16 obj_label_len;
	int len = 0;
	int is_revoked;
	u64 i;

	var.os = PLPKS_VAR_LINUX;
	var.component = PLPKS_WRAPKEY_COMPONENT;
	obj_labels_buf_ptr = obj_labels_buf;

	for (i = 0; i < obj_labels_count; ++i) {
		obj_label_len = get_unaligned_be16(obj_labels_buf_ptr);
		comp_prefix = obj_labels_buf_ptr +
			      PLPKS_OBJLABEL_LEN_FIELD_SIZE;

		var.namelen = obj_label_len - PLPKS_MAX_LABEL_ATTR_SIZE;
		var.name = kzalloc(var.namelen + 1, GFP_KERNEL);

		if (!var.name)
			return -ENOMEM;

		key_label_ptr = comp_prefix + PLPKS_MAX_LABEL_ATTR_SIZE;
		memcpy(var.name, key_label_ptr, var.namelen);

		is_revoked = plpks_is_wrapping_key_revoked(&var);
		if (is_revoked == revoked)
			len += sysfs_emit_at(buf, len, "<%s>\n",
					     (char *)var.name);
		else if (is_revoked < 0)
			pr_warn("Failed to get revocation status for <%s>\n",
				(char *)var.name);

		kfree(var.name);
		obj_labels_buf_ptr = key_label_ptr + var.namelen;
	}

	return len;
}

static ssize_t list_active_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	u8 *obj_labels_buf;
	u64 obj_labels_count = 0;
	int rc;

	rc = plpks_get_object_labels(&obj_labels_buf, &obj_labels_count,
				     PLPKS_WRAPKEY_COMPONENT);
	if (rc) {
		pr_err("Retrieving object labels failed. rc=%d\n", rc);
		goto out;
	}

	rc = list_key_labels(buf, obj_labels_buf, obj_labels_count,
			     PLPKS_WRAPKEY_UNREVOKED);

out:
	kfree(obj_labels_buf);
	return rc;
}

static ssize_t list_revoked_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	u8 *obj_labels_buf;
	u64 obj_labels_count = 0;
	int rc;

	rc = plpks_get_object_labels(&obj_labels_buf, &obj_labels_count,
				     PLPKS_WRAPKEY_COMPONENT);
	if (rc) {
		pr_err("Retrieving object labels failed. rc=%d\n", rc);
		goto out;
	}

	rc = list_key_labels(buf, obj_labels_buf, obj_labels_count,
			     PLPKS_WRAPKEY_REVOKED);

out:
	kfree(obj_labels_buf);
	return rc;
}

static ssize_t create_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	struct plpks_var var = {0};
	int rc;

	if (!capable(CAP_SYS_ADMIN)) {
		rc = -EPERM;
		goto out;
	}

	if (!count) {
		rc = -EINVAL;
		goto out;
	}

	if (count > PLPKS_MAX_NAME_SIZE) {
		rc = -ENAMETOOLONG;
		goto out;
	}

	if (!strcmp(buf, PLPKS_DEFAULT_WRAPKEY_LABEL)) {
		pr_warn("<%s> creation is restricted to pkwm init\n",
			PLPKS_DEFAULT_WRAPKEY_LABEL);
		rc = -EPERM;
		goto out;
	}

	if (!is_wrapkey_label_alnum(buf, count)) {
		rc = -EINVAL;
		goto out;
	}

	var.name = kstrndup(buf, count, GFP_KERNEL);
	if (!var.name) {
		rc = -ENOMEM;
		goto out;
	}

	var.namelen = count;
	var.policy = PLPKS_WRAPPINGKEY;
	var.os = PLPKS_VAR_LINUX;
	var.component = PLPKS_WRAPKEY_COMPONENT;

	rc = plpks_gen_wrapping_key(&var);
	if (rc) {
		pr_err("creation of wrapping key <%s> failed. rc = %d\n",
		       (char *)var.name, rc);
		goto out;
	}

	rc = count;
out:
	kfree(var.name);
	return rc;
}

static ssize_t revoke_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	struct plpks_var var = {0};
	int rc;

	if (!capable(CAP_SYS_ADMIN)) {
		rc = -EPERM;
		goto out;
	}

	if (!count) {
		rc = -EINVAL;
		goto out;
	}

	if (count > PLPKS_MAX_NAME_SIZE) {
		rc = -ENAMETOOLONG;
		goto out;
	}

	if (!strcmp(buf, PLPKS_DEFAULT_WRAPKEY_LABEL)) {
		pr_warn("<%s> must not be revoked\n",
			PLPKS_DEFAULT_WRAPKEY_LABEL);
		rc = -EPERM;
		goto out;
	}

	if (!is_wrapkey_label_alnum(buf, count)) {
		rc = -EINVAL;
		goto out;
	}

	var.name = kstrndup(buf, count, GFP_KERNEL);
	if (!var.name) {
		rc = -ENOMEM;
		goto out;
	}

	var.namelen = count;
	var.os = PLPKS_VAR_LINUX;
	var.component = PLPKS_WRAPKEY_COMPONENT;

	rc = plpks_revoke_wrapping_key(&var);

	if (rc) {
		pr_err("revocation of wrapping key <%s> failed. rc = %d\n",
		       (char *)var.name, rc);
		goto out;
	}

	rc = count;
out:
	kfree(var.name);
	return rc;
}

static ssize_t unrevoke_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	struct plpks_var var = {0};
	int rc;

	if (!capable(CAP_SYS_ADMIN)) {
		rc = -EPERM;
		goto out;
	}

	if (!count) {
		rc = -EINVAL;
		goto out;
	}

	if (count > PLPKS_MAX_NAME_SIZE) {
		rc = -ENAMETOOLONG;
		goto out;
	}

	if (!strcmp(buf, PLPKS_DEFAULT_WRAPKEY_LABEL)) {
		pr_warn("unrevoke on <%s> is invalid\n",
			PLPKS_DEFAULT_WRAPKEY_LABEL);
		rc = -EINVAL;
		goto out;
	}

	if (!is_wrapkey_label_alnum(buf, count)) {
		rc = -EINVAL;
		goto out;
	}

	var.name = kstrndup(buf, count, GFP_KERNEL);
	if (!var.name) {
		rc = -ENOMEM;
		goto out;
	}

	var.namelen = count;
	var.os = PLPKS_VAR_LINUX;
	var.component = PLPKS_WRAPKEY_COMPONENT;

	rc = plpks_unrevoke_wrapping_key(&var);

	if (rc) {
		pr_err("un-revocation of wrapping key <%s> failed. rc = %d\n",
		       (char *)var.name, rc);
		goto out;
	}

	rc = count;
out:
	kfree(var.name);
	return rc;
}

static ssize_t delete_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	struct plpks_var var = {0};
	int rc;

	if (!capable(CAP_SYS_ADMIN)) {
		rc = -EPERM;
		goto out;
	}

	if (!count) {
		rc = -EINVAL;
		goto out;
	}

	if (count > PLPKS_MAX_NAME_SIZE) {
		rc = -ENAMETOOLONG;
		goto out;
	}

	if (!strcmp(buf, PLPKS_DEFAULT_WRAPKEY_LABEL)) {
		pr_warn("<%s> must not be deleted\n",
			PLPKS_DEFAULT_WRAPKEY_LABEL);
		rc = -EPERM;
		goto out;
	}

	if (!is_wrapkey_label_alnum(buf, count)) {
		rc = -EINVAL;
		goto out;
	}

	var.name = kstrndup(buf, count, GFP_KERNEL);
	if (!var.name) {
		rc = -ENOMEM;
		goto out;
	}

	var.namelen = count;
	var.os = PLPKS_VAR_LINUX;
	var.component = PLPKS_WRAPKEY_COMPONENT;

	rc = plpks_del_wrapping_key(&var);

	if (rc) {
		pr_err("deletion of wrapping key <%s> failed. rc = %d\n",
		       (char *)var.name, rc);
		goto out;
	}

	rc = count;
out:
	kfree(var.name);
	return rc;
}

static struct kobj_attribute view_active_attr = __ATTR_RO(list_active);

static struct kobj_attribute view_revoked_attr = __ATTR_RO(list_revoked);

static struct kobj_attribute create_attr = __ATTR_WO(create);

static struct kobj_attribute revoke_attr = __ATTR_WO(revoke);

static struct kobj_attribute unrevoke_attr = __ATTR_WO(unrevoke);

static struct kobj_attribute delete_attr = __ATTR_WO(delete);

static struct attribute *wrapkey_attrs[] = {
	&view_active_attr.attr,
	&view_revoked_attr.attr,
	&create_attr.attr,
	&revoke_attr.attr,
	&unrevoke_attr.attr,
	&delete_attr.attr,
	NULL,
};

static const struct attribute_group wrapkey_attr_group = {
	.attrs = wrapkey_attrs,
};
__ATTRIBUTE_GROUPS(wrapkey_attr);

static const struct kobj_type wrapkey_ktype = {
	.sysfs_ops	= &kobj_sysfs_ops,
	.default_groups = wrapkey_attr_groups,
};

static __init int wrapkey_sysfs_init(void)
{
	int rc;

	if (!plpks_revoke_is_supported()) {
		pr_err("H_PKS_{UN}REVOKE_OBJECT interface not supported\n");
		return -ENODEV;
	}

	wrapkey_kobj = kzalloc_obj(*wrapkey_kobj);
	if (!wrapkey_kobj)
		return -ENOMEM;

	rc = plpks_init_child_kobj(wrapkey_kobj, &wrapkey_ktype, "wrapkey");

	if (rc)
		kobject_put(wrapkey_kobj);

	return rc;
}

late_initcall(wrapkey_sysfs_init);
