// SPDX-License-Identifier: GPL-2.0
/*
 * Per-function short-circuit mitigation.
 *
 * Copyright (C) 2026 Sasha Levin <sashal@kernel.org>
 *
 * Engaging a killswitch installs a kprobe at the function's entry
 * whose pre-handler sets the return register and skips the body via
 * override_function_with_return().  Operator interface lives at
 * /sys/kernel/security/killswitch/.
 */

#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/ctype.h>
#include <linux/error-injection.h>
#include <linux/init.h>
#include <linux/killswitch.h>
#include <linux/kprobes.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/panic.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/security.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>

struct ks_attr {
	struct list_head	list;
	struct kprobe		kp;
	/* atomic so a writer racing an in-flight call can't tear the long. */
	atomic_long_t		retval;
	/* false once disengaged; per-fn file ops then return -EIDRM. */
	bool			engaged;
	unsigned long __percpu	*hits;
	struct dentry		*dir;
	/* engaged_list holds one ref; each open per-fn fd holds one. */
	struct kref		refcnt;
};

static DEFINE_MUTEX(ks_lock);
static LIST_HEAD(ks_engaged_list);
static struct dentry *ks_root_dir;
static struct dentry *ks_fn_dir;	/* parent for per-fn directories */

/* ------------------------------------------------------------------ *
 * Pre-handler: the actual override                                   *
 * ------------------------------------------------------------------ */

static int ks_kprobe_pre_handler(struct kprobe *kp, struct pt_regs *regs)
{
	struct ks_attr *attr = container_of(kp, struct ks_attr, kp);

	this_cpu_inc(*attr->hits);
	regs_set_return_value(regs, (unsigned long)atomic_long_read(&attr->retval));
	override_function_with_return(regs);
	return 1;
}
NOKPROBE_SYMBOL(ks_kprobe_pre_handler);

/* Defined non-NULL so the kprobe layer keeps the IPMODIFY ops. */
static void ks_kprobe_post_handler(struct kprobe *kp, struct pt_regs *regs,
				   unsigned long flags)
{
}

/* ------------------------------------------------------------------ *
 * Attribute lifecycle                                                *
 * ------------------------------------------------------------------ */

static struct ks_attr *ks_attr_lookup(const char *symbol)
{
	struct ks_attr *attr;

	list_for_each_entry(attr, &ks_engaged_list, list)
		if (!strcmp(attr->kp.symbol_name, symbol))
			return attr;
	return NULL;
}

static unsigned long ks_attr_hits(const struct ks_attr *attr)
{
	unsigned long total = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		total += *per_cpu_ptr(attr->hits, cpu);
	return total;
}

static void ks_attr_destroy(struct ks_attr *attr)
{
	if (!attr)
		return;
	free_percpu(attr->hits);
	kfree(attr->kp.symbol_name);
	kfree(attr);
}

static void ks_attr_kref_release(struct kref *kref)
{
	ks_attr_destroy(container_of(kref, struct ks_attr, refcnt));
}

static void ks_attr_get(struct ks_attr *attr)
{
	kref_get(&attr->refcnt);
}

static void ks_attr_put(struct ks_attr *attr)
{
	kref_put(&attr->refcnt, ks_attr_kref_release);
}

static struct ks_attr *ks_attr_alloc(const char *symbol)
{
	struct ks_attr *attr;

	attr = kzalloc(sizeof(*attr), GFP_KERNEL);
	if (!attr)
		return NULL;

	attr->kp.symbol_name = kstrdup(symbol, GFP_KERNEL);
	if (!attr->kp.symbol_name)
		goto err;

	attr->hits = alloc_percpu(unsigned long);
	if (!attr->hits)
		goto err;

	attr->kp.pre_handler = ks_kprobe_pre_handler;
	attr->kp.post_handler = ks_kprobe_post_handler;
	INIT_LIST_HEAD(&attr->list);
	kref_init(&attr->refcnt);
	return attr;

err:
	ks_attr_destroy(attr);
	return NULL;
}

/* ------------------------------------------------------------------ *
 * Securityfs: per-fn attribute files                                 *
 * ------------------------------------------------------------------ */

/*
 * Look up by symbol name (the parent dentry's basename) under
 * ks_lock and confirm attr->dir is the file's parent dentry.  This
 * binds the fd to the engagement it was opened against and avoids
 * dereferencing inode->i_private, which a racing disengage may have
 * freed.  d_parent is stable for the open's lifetime via the file's
 * dentry reference.
 */
static int ks_attr_open(struct inode *inode, struct file *file)
{
	struct dentry *parent = file->f_path.dentry->d_parent;
	const char *name = parent->d_name.name;
	struct ks_attr *attr;

	mutex_lock(&ks_lock);
	attr = ks_attr_lookup(name);
	if (attr && attr->dir == parent)
		ks_attr_get(attr);
	else
		attr = NULL;
	mutex_unlock(&ks_lock);
	if (!attr)
		return -ENOENT;
	file->private_data = attr;
	return 0;
}

static int ks_attr_release(struct inode *inode, struct file *file)
{
	ks_attr_put(file->private_data);
	file->private_data = NULL;
	return 0;
}

/* Caller must hold ks_lock. */
static int ks_attr_check_live(const struct ks_attr *attr)
{
	return attr->engaged ? 0 : -EIDRM;
}

static ssize_t ks_retval_read(struct file *file, char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct ks_attr *attr = file->private_data;
	char buf[32];
	long val;
	int ret, len;

	mutex_lock(&ks_lock);
	ret = ks_attr_check_live(attr);
	val = atomic_long_read(&attr->retval);
	mutex_unlock(&ks_lock);
	if (ret)
		return ret;
	len = scnprintf(buf, sizeof(buf), "%ld\n", val);
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static ssize_t ks_retval_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	struct ks_attr *attr = file->private_data;
	char buf[32];
	long val;
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	strim(buf);

	ret = kstrtol(buf, 0, &val);
	if (ret)
		return ret;

	mutex_lock(&ks_lock);
	ret = ks_attr_check_live(attr);
	if (!ret)
		atomic_long_set(&attr->retval, val);
	mutex_unlock(&ks_lock);

	return ret ? ret : count;
}

static const struct file_operations ks_retval_fops = {
	.open		= ks_attr_open,
	.release	= ks_attr_release,
	.read		= ks_retval_read,
	.write	= ks_retval_write,
	.llseek	= default_llseek,
};

static ssize_t ks_hits_read(struct file *file, char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	struct ks_attr *attr = file->private_data;
	char buf[32];
	unsigned long hits;
	int ret, len;

	mutex_lock(&ks_lock);
	ret = ks_attr_check_live(attr);
	hits = ks_attr_hits(attr);
	mutex_unlock(&ks_lock);
	if (ret)
		return ret;
	len = scnprintf(buf, sizeof(buf), "%lu\n", hits);
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static const struct file_operations ks_hits_fops = {
	.open		= ks_attr_open,
	.release	= ks_attr_release,
	.read		= ks_hits_read,
	.llseek		= default_llseek,
};

static int ks_create_attr_dir(struct ks_attr *attr)
{
	struct dentry *d;

	attr->dir = securityfs_create_dir(attr->kp.symbol_name, ks_fn_dir);
	if (IS_ERR(attr->dir))
		return PTR_ERR(attr->dir);

	/* ks_attr_open looks the attr up by name; i_private is unused. */
	d = securityfs_create_file("retval", 0600, attr->dir,
				   NULL, &ks_retval_fops);
	if (IS_ERR(d))
		goto err;
	d = securityfs_create_file("hits", 0400, attr->dir,
				   NULL, &ks_hits_fops);
	if (IS_ERR(d))
		goto err;
	return 0;
err:
	securityfs_remove(attr->dir);
	attr->dir = NULL;
	return PTR_ERR(d);
}

/* ------------------------------------------------------------------ *
 * Engage / disengage                                                 *
 * ------------------------------------------------------------------ */

static int __ks_engage(const char *symbol, long retval, bool from_cmdline)
{
	struct ks_attr *attr;
	int ret;

	if (!symbol || !*symbol)
		return -EINVAL;

	if (!from_cmdline) {
		ret = security_locked_down(LOCKDOWN_KILLSWITCH);
		if (ret)
			return ret;
	}

	mutex_lock(&ks_lock);

	if (ks_attr_lookup(symbol)) {
		ret = -EBUSY;
		goto out_unlock;
	}

	attr = ks_attr_alloc(symbol);
	if (!attr) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	atomic_long_set(&attr->retval, retval);

	ret = register_kprobe(&attr->kp);
	if (ret) {
		pr_warn("killswitch: register_kprobe(%s) failed: %d\n",
			symbol, ret);
		ks_attr_put(attr);
		goto out_unlock;
	}

	ret = ks_create_attr_dir(attr);
	if (ret) {
		unregister_kprobe(&attr->kp);
		ks_attr_put(attr);
		goto out_unlock;
	}

	list_add_tail(&attr->list, &ks_engaged_list);
	attr->engaged = true;
	add_taint(TAINT_KILLSWITCH, LOCKDEP_STILL_OK);

	if (from_cmdline) {
		pr_warn("killswitch: engage %s=%ld source=cmdline\n",
			symbol, retval);
	} else {
		pr_warn("killswitch: engage %s=%ld uid=%u auid=%u ses=%u comm=%s\n",
			symbol, retval,
			from_kuid(&init_user_ns, current_uid()),
			from_kuid(&init_user_ns, audit_get_loginuid(current)),
			audit_get_sessionid(current),
			current->comm);
	}
	ret = 0;

out_unlock:
	mutex_unlock(&ks_lock);
	return ret;
}

int killswitch_engage(const char *symbol, long retval)
{
	return __ks_engage(symbol, retval, false);
}

static int __ks_disengage(const char *symbol)
{
	struct ks_attr *attr;
	unsigned long hits;
	int ret = 0;

	mutex_lock(&ks_lock);
	attr = ks_attr_lookup(symbol);
	if (!attr) {
		ret = -ENOENT;
		goto out_unlock;
	}

	unregister_kprobe(&attr->kp);
	attr->engaged = false;
	list_del(&attr->list);
	hits = ks_attr_hits(attr);
	securityfs_remove(attr->dir);

	pr_warn("killswitch: disengage %s hits=%lu uid=%u auid=%u ses=%u comm=%s\n",
		symbol, hits,
		from_kuid(&init_user_ns, current_uid()),
		from_kuid(&init_user_ns, audit_get_loginuid(current)),
		audit_get_sessionid(current),
		current->comm);

	/* unregister_kprobe() already waited out in-flight pre-handlers. */
	ks_attr_put(attr);

out_unlock:
	mutex_unlock(&ks_lock);
	return ret;
}

int killswitch_disengage(const char *symbol)
{
	return __ks_disengage(symbol);
}

bool killswitch_is_engaged(const char *symbol)
{
	bool engaged;

	mutex_lock(&ks_lock);
	engaged = ks_attr_lookup(symbol) != NULL;
	mutex_unlock(&ks_lock);
	return engaged;
}

static void ks_disengage_all_locked(void)
{
	struct ks_attr *attr, *n;

	list_for_each_entry_safe(attr, n, &ks_engaged_list, list) {
		unregister_kprobe(&attr->kp);
		attr->engaged = false;
		list_del(&attr->list);
		securityfs_remove(attr->dir);
		pr_warn("killswitch: disengage %s hits=%lu (disengage_all)\n",
			attr->kp.symbol_name, ks_attr_hits(attr));
		ks_attr_put(attr);
	}
}

/* ------------------------------------------------------------------ *
 * Module unload: drop engagements on functions in the going module   *
 * ------------------------------------------------------------------ */

static int ks_module_notify(struct notifier_block *nb, unsigned long action,
			    void *data)
{
	struct module *mod = data;
	struct ks_attr *attr, *n;

	if (action != MODULE_STATE_GOING)
		return NOTIFY_DONE;

	mutex_lock(&ks_lock);
	list_for_each_entry_safe(attr, n, &ks_engaged_list, list) {
		if (!attr->kp.addr ||
		    __module_address((unsigned long)attr->kp.addr) != mod)
			continue;

		pr_warn("killswitch: %s mitigation lost: module %s unloading; re-engage after reload if still needed\n",
			attr->kp.symbol_name, mod->name);
		unregister_kprobe(&attr->kp);
		attr->engaged = false;
		list_del(&attr->list);
		securityfs_remove(attr->dir);
		ks_attr_put(attr);
	}
	mutex_unlock(&ks_lock);
	return NOTIFY_DONE;
}

static struct notifier_block ks_module_nb = {
	.notifier_call = ks_module_notify,
};

/* ------------------------------------------------------------------ *
 * Top-level securityfs files: control / engaged / taint              *
 * ------------------------------------------------------------------ */

static int ks_engaged_show(struct seq_file *m, void *v)
{
	struct ks_attr *attr;

	mutex_lock(&ks_lock);
	list_for_each_entry(attr, &ks_engaged_list, list) {
		seq_printf(m, "%s retval=%ld hits=%lu\n",
			   attr->kp.symbol_name,
			   atomic_long_read(&attr->retval),
			   ks_attr_hits(attr));
	}
	mutex_unlock(&ks_lock);
	return 0;
}

static int ks_engaged_open(struct inode *inode, struct file *file)
{
	return single_open(file, ks_engaged_show, NULL);
}

static const struct file_operations ks_engaged_fops = {
	.open		= ks_engaged_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static ssize_t ks_taint_read(struct file *file, char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	char buf[4];
	int len;

	len = scnprintf(buf, sizeof(buf), "%d\n",
			test_taint(TAINT_KILLSWITCH) ? 1 : 0);
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static const struct file_operations ks_taint_fops = {
	.open	= simple_open,
	.read	= ks_taint_read,
	.llseek	= default_llseek,
};

/*
 * control: parse one of:
 *   engage <symbol> <retval>
 *   disengage <symbol>
 *   disengage_all
 */
static ssize_t ks_control_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	char *buf, *cur, *verb, *sym, *retstr;
	long retval = 0;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count == 0 || count > 4096)
		return -EINVAL;

	buf = memdup_user_nul(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	cur = strim(buf);
	verb = strsep(&cur, " \t\n");
	if (!verb || !*verb) {
		ret = -EINVAL;
		goto out;
	}

	if (!strcmp(verb, "disengage_all")) {
		mutex_lock(&ks_lock);
		ks_disengage_all_locked();
		mutex_unlock(&ks_lock);
		ret = count;
		goto out;
	}

	sym = strsep(&cur, " \t\n");
	if (!sym || !*sym) {
		ret = -EINVAL;
		goto out;
	}

	if (!strcmp(verb, "disengage")) {
		ret = __ks_disengage(sym);
		ret = ret ? ret : count;
		goto out;
	}

	if (strcmp(verb, "engage")) {
		ret = -EINVAL;
		goto out;
	}

	retstr = strsep(&cur, " \t\n");
	if (!retstr || !*retstr) {
		ret = -EINVAL;
		goto out;
	}
	if (kstrtol(retstr, 0, &retval)) {
		ret = -EINVAL;
		goto out;
	}

	ret = killswitch_engage(sym, retval);
	if (!ret)
		ret = count;

out:
	kfree(buf);
	return ret;
}

static const struct file_operations ks_control_fops = {
	.open	= simple_open,
	.write	= ks_control_write,
	.llseek	= noop_llseek,
};

/* ------------------------------------------------------------------ *
 * Boot parameter:                                                    *
 *   killswitch=fn1=-1,fn2=0,fn3=-22                                  *
 * ------------------------------------------------------------------ */

#define KS_BOOT_BUF 1024
static char ks_boot_buf[KS_BOOT_BUF] __initdata;
static bool ks_boot_present __initdata;

static int __init ks_boot_setup(char *str)
{
	if (!str)
		return 0;
	strscpy(ks_boot_buf, str, sizeof(ks_boot_buf));
	ks_boot_present = true;
	return 1;
}
__setup("killswitch=", ks_boot_setup);

static void __init ks_apply_boot_params(void)
{
	char *cur, *tok;
	long retval;

	if (!ks_boot_present)
		return;

	cur = ks_boot_buf;
	while ((tok = strsep(&cur, ",")) != NULL) {
		char *eq, *sym, *retstr;

		if (!*tok)
			continue;
		eq = strchr(tok, '=');
		if (!eq) {
			pr_warn("killswitch: cmdline missing '=': %s\n", tok);
			continue;
		}
		*eq++ = '\0';
		sym = tok;
		retstr = eq;

		if (kstrtol(retstr, 0, &retval)) {
			pr_warn("killswitch: cmdline bad retval %s=%s\n",
				sym, retstr);
			continue;
		}

		if (__ks_engage(sym, retval, true))
			pr_warn("killswitch: cmdline engage %s failed\n", sym);
	}
}

/* ------------------------------------------------------------------ *
 * Init                                                               *
 * ------------------------------------------------------------------ */

static int __init killswitch_init(void)
{
	struct dentry *d;

	ks_root_dir = securityfs_create_dir("killswitch", NULL);
	if (IS_ERR(ks_root_dir))
		return PTR_ERR(ks_root_dir);

	d = securityfs_create_file("control", 0200, ks_root_dir,
				   NULL, &ks_control_fops);
	if (IS_ERR(d))
		goto err;
	d = securityfs_create_file("engaged", 0444, ks_root_dir,
				   NULL, &ks_engaged_fops);
	if (IS_ERR(d))
		goto err;
	d = securityfs_create_file("taint", 0444, ks_root_dir,
				   NULL, &ks_taint_fops);
	if (IS_ERR(d))
		goto err;

	ks_fn_dir = securityfs_create_dir("fn", ks_root_dir);
	if (IS_ERR(ks_fn_dir)) {
		d = ks_fn_dir;
		goto err;
	}

	register_module_notifier(&ks_module_nb);
	ks_apply_boot_params();

	pr_info("killswitch: ready (sysfs at /sys/kernel/security/killswitch/)\n");
	return 0;

err:
	securityfs_remove(ks_root_dir);
	return PTR_ERR(d);
}
late_initcall(killswitch_init);

/* ------------------------------------------------------------------ *
 * KUnit tests                                                        *
 * ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_KUNIT)
#include <kunit/test.h>

/* Non-static so kallsyms resolves them without CONFIG_KALLSYMS_ALL. */
int ks_kunit_target_int(int x);
void *ks_kunit_target_ptr(int x);

#if __has_attribute(__noipa__)
# define KS_KUNIT_NOIPA __attribute__((__noipa__))
#else
# define KS_KUNIT_NOIPA noinline __noclone
#endif

KS_KUNIT_NOIPA int ks_kunit_target_int(int x)
{
	return x + 1;
}

KS_KUNIT_NOIPA void *ks_kunit_target_ptr(int x)
{
	return ERR_PTR(-EIO);
}

static int ks_kunit_init(struct kunit *test)
{
	if (security_locked_down(LOCKDOWN_KILLSWITCH))
		kunit_skip(test, "integrity lockdown blocks killswitch_engage()");
	return 0;
}

static int ks_kunit_init_lockdown(struct kunit *test)
{
	if (!security_locked_down(LOCKDOWN_KILLSWITCH))
		kunit_skip(test, "requires lockdown=integrity");
	return 0;
}

static void ks_disengage_quiet(const char *sym)
{
	if (killswitch_is_engaged(sym))
		killswitch_disengage(sym);
}

static void ks_test_engage_int(struct kunit *test)
{
	int ret;

	ret = killswitch_engage("ks_kunit_target_int", -EPERM);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ks_kunit_target_int(7), -EPERM);
	KUNIT_EXPECT_EQ(test, killswitch_disengage("ks_kunit_target_int"), 0);
	KUNIT_EXPECT_EQ(test, ks_kunit_target_int(7), 8);
}

static void ks_test_double_engage(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test,
		killswitch_engage("ks_kunit_target_int", 0), 0);
	KUNIT_EXPECT_EQ(test,
		killswitch_engage("ks_kunit_target_int", 0), -EBUSY);
	ks_disengage_quiet("ks_kunit_target_int");
}

static void ks_test_disengage_unknown(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
		killswitch_disengage("ks_kunit_target_int"), -ENOENT);
}

static void ks_test_pointer_target(struct kunit *test)
{
	long retval = (long)(unsigned long)ERR_PTR(-EACCES);

	KUNIT_ASSERT_EQ(test,
		killswitch_engage("ks_kunit_target_ptr", retval), 0);
	KUNIT_EXPECT_TRUE(test, IS_ERR(ks_kunit_target_ptr(0)));
	KUNIT_EXPECT_EQ(test, PTR_ERR(ks_kunit_target_ptr(0)), -EACCES);
	ks_disengage_quiet("ks_kunit_target_ptr");
}

static void ks_test_taint_set(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test,
		killswitch_engage("ks_kunit_target_int", 0), 0);
	KUNIT_EXPECT_TRUE(test, test_taint(TAINT_KILLSWITCH));
	ks_disengage_quiet("ks_kunit_target_int");
	/* taint must persist even after disengage */
	KUNIT_EXPECT_TRUE(test, test_taint(TAINT_KILLSWITCH));
}

static void ks_test_hits_counter(struct kunit *test)
{
	struct ks_attr *attr;
	int i;

	KUNIT_ASSERT_EQ(test,
		killswitch_engage("ks_kunit_target_int", 0), 0);

	for (i = 0; i < 17; i++)
		(void)ks_kunit_target_int(i);

	mutex_lock(&ks_lock);
	attr = ks_attr_lookup("ks_kunit_target_int");
	KUNIT_EXPECT_NOT_NULL(test, attr);
	if (attr)
		KUNIT_EXPECT_EQ(test, ks_attr_hits(attr), 17UL);
	mutex_unlock(&ks_lock);

	ks_disengage_quiet("ks_kunit_target_int");
}

static struct kunit_case ks_kunit_cases[] = {
	KUNIT_CASE(ks_test_engage_int),
	KUNIT_CASE(ks_test_double_engage),
	KUNIT_CASE(ks_test_disengage_unknown),
	KUNIT_CASE(ks_test_pointer_target),
	KUNIT_CASE(ks_test_taint_set),
	KUNIT_CASE(ks_test_hits_counter),
	{}
};

static struct kunit_suite ks_kunit_suite = {
	.name = "killswitch",
	.init = ks_kunit_init,
	.test_cases = ks_kunit_cases,
};

/*
 * Lockdown suite. Skipped unless the kernel was booted with
 * lockdown=integrity (or higher). Run together with
 * killswitch=ks_kunit_target_int=... on the same cmdline to also
 * exercise the cmdline-bypass and disengage-under-lockdown paths.
 */
static void ks_test_lockdown_runtime_engage(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
		killswitch_engage("ks_kunit_target_int", 0), -EPERM);
}

static void ks_test_lockdown_cmdline_disengage(struct kunit *test)
{
	if (!killswitch_is_engaged("ks_kunit_target_int"))
		kunit_skip(test,
			   "requires killswitch=ks_kunit_target_int=... on cmdline");
	KUNIT_EXPECT_EQ(test,
		killswitch_disengage("ks_kunit_target_int"), 0);
}

static struct kunit_case ks_kunit_lockdown_cases[] = {
	KUNIT_CASE(ks_test_lockdown_runtime_engage),
	KUNIT_CASE(ks_test_lockdown_cmdline_disengage),
	{}
};

static struct kunit_suite ks_kunit_lockdown_suite = {
	.name = "killswitch_lockdown",
	.init = ks_kunit_init_lockdown,
	.test_cases = ks_kunit_lockdown_cases,
};

kunit_test_suites(&ks_kunit_suite, &ks_kunit_lockdown_suite);

#endif /* CONFIG_KUNIT */

