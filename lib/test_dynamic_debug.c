// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel module to test/demonstrate dynamic_debug features,
 * particularly classmaps and their support for subsystems like DRM.
 *
 * Authors:
 *      Jim Cromie	<jim.cromie@gmail.com>
 */

/*
 * This file is built 2x, also making test_dynamic_debug_submod.ko,
 * whose 2-line src file #includes this file.  This gives us a _submod
 * clone with identical pr_debugs, without further maintenance.
 *
 * If things are working properly, they should operate identically
 * when printed or adjusted by >control.  This eases visual perusal of
 * the logs, and simplifies testing, by easing the proper accounting
 * of expectations.
 *
 * It also puts both halves of the subsystem _DEFINE & _USE use case
 * together, and integrates the common ENUM providing both class_ids
 * and class-names to both _DEFINErs and _USERs.  I think this makes
 * the usage clearer.
 */
#if defined(TEST_DYNAMIC_DEBUG_SUBMOD)
  #define pr_fmt(fmt) "test_dd_submod: %s " fmt, __func__
#else
  #define pr_fmt(fmt) "test_dd: " fmt
#endif

#include <linux/module.h>
#include <linux/sched.h>

/* re-trigger debug output by reading or writing sysfs nodes: do_classes or do_bulk */
static void do_classes(unsigned int); /* device under test */
static void do_bulk(unsigned int);    /* device under test */

static int param_set_do_repeats(const char *instr, const struct kernel_param *kp)
{
	int rc;
	unsigned int ct;
	void (*repeat_fn)(unsigned int) = kp->arg;

	rc = kstrtouint(instr, 0, &ct);
	if (rc) {
		pr_err("expecting numeric input, using 1 instead\n");
		ct = 1;
	}

	repeat_fn(ct);

	return 0;
}

static int param_get_do_repeats(char *buffer, const struct kernel_param *kp)
{
	void (*repeat_fn)(unsigned int) = kp->arg;

	repeat_fn(1);

	return scnprintf(buffer, PAGE_SIZE, "did 1 %s\n", kp->name);
}

static const struct kernel_param_ops param_ops_do_repeats = {
	.set = param_set_do_repeats,
	.get = param_get_do_repeats,
};

module_param_cb(do_classes, &param_ops_do_repeats, do_classes, 0600);
module_param_cb(do_bulk, &param_ops_do_repeats, do_bulk, 0600);

/*
 * Demonstrate/test DISJOINT & LEVEL typed classmaps with a sys-param.
 *
 * To comport with DRM debug-category (an int), classmaps map names to
 * ids (also an int).  So a classmap starts with an enum; DRM has enum
 * debug_category: with DRM_UT_<CORE,DRIVER,KMS,etc>.  We use the enum
 * values as class-ids, and stringified enum-symbols as classnames.
 *
 * Modules with multiple CLASSMAPS must have enums with distinct
 * value-ranges, as arranged below with explicit enum_sym = X inits.
 * To clarify this sharing, declare the 2 enums now, for the 2
 * different classmap types
 */

/* numeric input, independent bits */
enum cat_disjoint_bits {
	D2_CORE = 0,
	D2_DRIVER,
	D2_KMS,
	D2_PRIME,
	D2_ATOMIC,
	D2_VBL,
	D2_STATE,
	D2_LEASE,
	D2_DP,
	D2_DRMRES };

/* numeric verbosity, V2 > V1 related.  V1 is > D2_DRMRES */
enum cat_level_num { V1 = 16, V2, V3, V4, V5, V6, V7, V8 };

/* test _USE_ w offset */
enum cat_level_offset { Vu1 = V1 + 8, Vu2, Vu3, Vu4, Vu5, Vu6, Vu7, Vu8 };

/*
 * use/demonstrate multi-module-group classmaps, as for DRM
 */
#if !defined(TEST_DYNAMIC_DEBUG_SUBMOD)
/*
 * For module-groups of 1+, define classmaps with names (stringified
 * enum-symbols) copied from above. 1-to-1 mapping is recommended.
 * The classmap is exported, so that other modules in the group can
 * link to it and control their prdbgs.
 */
DYNAMIC_DEBUG_CLASSMAP_DEFINE(map_disjoint_bits, DD_CLASS_TYPE_DISJOINT_BITS,
			      D2_CORE,
			      "D2_CORE",
			      "D2_DRIVER",
			      "D2_KMS",
			      "D2_PRIME",
			      "D2_ATOMIC",
			      "D2_VBL",
			      "D2_STATE",
			      "D2_LEASE",
			      "D2_DP",
			      "D2_DRMRES");

DYNAMIC_DEBUG_CLASSMAP_DEFINE(map_level_num, DD_CLASS_TYPE_LEVEL_NUM,
			      V1, "V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8");

/*
 * for use-cases that want it, provide a sysfs-param to set the
 * classes in the classmap.  It is at this interface where the
 * "v3>v2" property is applied to DD_CLASS_TYPE_LEVEL_NUM inputs.
 */

DYNAMIC_DEBUG_CLASSMAP_PARAM(p_disjoint_bits,	map_disjoint_bits, p);
DYNAMIC_DEBUG_CLASSMAP_PARAM(p_level_num,	map_level_num, p);

#ifdef FORCE_CLASSID_CONFLICT
/*
 * Enable with -Dflag on compile to test overlapping class-id range
 * detection.  This should warn on modprobes.
 */
DYNAMIC_DEBUG_CLASSMAP_DEFINE(classid_range_conflict, 0, D2_CORE + 1, "D3_CORE");
#endif


#if defined(DD_MACRO_ARGCHECK)
/*
 * Exersize compile-time arg-checks in DYNAMIC_DEBUG_CLASSMAP_DEFINE.
 * These will break compilation.
 */
DYNAMIC_DEBUG_CLASSMAP_DEFINE(fail_base_neg, 0, -1, "NEGATIVE_BASE_ARG");
DYNAMIC_DEBUG_CLASSMAP_DEFINE(fail_base_big, 0, 100, "TOOBIG_BASE_ARG");
DYNAMIC_DEBUG_CLASSMAP_DEFINE(fail_str_type, 0, 0, 1 /* not a string */);
DYNAMIC_DEBUG_CLASSMAP_DEFINE(fail_emptyclass, 0, 0 /* ,empty */);
DYNAMIC_DEBUG_CLASSMAP_DEFINE(fail_maptype, 3, 10, "no such type");
DYNAMIC_DEBUG_CLASSMAP_DEFINE(fail_base_len, 0, 60,
			      "base", "plus", "classes", "length", "too-big");
#endif

#else /* TEST_DYNAMIC_DEBUG_SUBMOD */
/*
 * In submod (drm-drivers/helpers) use the classmaps defined in
 * top/parent module above.  We _USE_() with offset, to test the
 * non-zero case.
 */
DYNAMIC_DEBUG_CLASSMAP_USE(map_disjoint_bits);
/*
 * maybe force failure of runtime sanity test of classmap.length + offset < 63
 */
#if !defined(DD_RUNTIME_CLASS_CHECK)
DYNAMIC_DEBUG_CLASSMAP_USE_(map_level_num, 8);
#else
DYNAMIC_DEBUG_CLASSMAP_USE_(map_level_num, 55);
#endif

#if defined(DD_MACRO_ARGCHECK)
DYNAMIC_DEBUG_CLASSMAP_USE_(fail_offset_big, 100);
#endif /* DD_MACRO_ARGCHECK */

#endif /* TEST_DYNAMIC_DEBUG_SUBMOD */

/*
 * now add the sysfs-params to both sub/super-mods
 */

/* stand-in for all pr_debug etc */
#define prdbg(SYM) __pr_debug_cls(SYM, #SYM " msg\n")

static void do_cats(void)
{
	pr_debug("doing categories\n");

	prdbg(D2_CORE);
	prdbg(D2_DRIVER);
	prdbg(D2_KMS);
	prdbg(D2_PRIME);
	prdbg(D2_ATOMIC);
	prdbg(D2_VBL);
	prdbg(D2_STATE);
	prdbg(D2_LEASE);
	prdbg(D2_DP);
	prdbg(D2_DRMRES);
}

static void do_levels(void)
{
	pr_debug("doing levels\n");

#if !defined(TEST_DYNAMIC_DEBUG_SUBMOD)
	prdbg(V1);
	prdbg(V2);
	prdbg(V3);
	prdbg(V4);
	prdbg(V5);
	prdbg(V6);
	prdbg(V7);
	prdbg(V8);
#else
	prdbg(Vu1);
	prdbg(Vu2);
	prdbg(Vu3);
	prdbg(Vu4);
	prdbg(Vu5);
	prdbg(Vu6);
	prdbg(Vu7);
	prdbg(Vu8);
#endif
}

static void do_classes(unsigned int ct)
{
	/* maybe clamp this */
	pr_debug("do_classes %u times:\n", ct);
	for (; ct; ct--) {
		do_cats();
		do_levels();
		cond_resched();
	}
}

static void do_bulk(unsigned int ct)
{
	unsigned int i;

	pr_debug("do_bulk %u times:\n", ct);
	for (i = 0; i < ct; i++) {
		pr_debug("bulk msg %u.0\n", i + 1);
		pr_debug("bulk msg %u.1\n", i + 1);
		pr_debug("bulk msg %u.2\n", i + 1);
		pr_debug("bulk msg %u.3\n", i + 1);
		pr_debug("bulk msg %u.4\n", i + 1);
		pr_debug("bulk msg %u.5\n", i + 1);
		pr_debug("bulk msg %u.6\n", i + 1);
		pr_debug("bulk msg %u.7\n", i + 1);
		pr_debug("bulk msg %u.8\n", i + 1);
		pr_debug("bulk msg %u.9\n", i + 1);
		cond_resched();
	}
}

static int __init test_dynamic_debug_init(void)
{
	pr_debug("init start\n");
	do_classes(1);
	do_bulk(1);
	pr_debug("init done\n");
	return 0;
}

static void __exit test_dynamic_debug_exit(void)
{
	pr_debug_ratelimited("exited\n");
}

module_init(test_dynamic_debug_init);
module_exit(test_dynamic_debug_exit);

MODULE_AUTHOR("Jim Cromie <jim.cromie@gmail.com>");
MODULE_DESCRIPTION("test/demonstrate dynamic-debug features");
MODULE_LICENSE("GPL");
