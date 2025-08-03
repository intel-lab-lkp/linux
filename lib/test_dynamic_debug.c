// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel module for testing dynamic_debug
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
  #define pr_fmt(fmt) "test_dd_submod: " fmt
#else
  #define pr_fmt(fmt) "test_dd: " fmt
#endif

#include <linux/module.h>

/* re-trigger debug output by reading or writing sysfs node: do_prints */
#define PRINT_CLAMP 10000
static void do_prints(unsigned int); /* device under test */
static int param_set_do_prints(const char *instr, const struct kernel_param *kp)
{
	int rc;
	unsigned int ct;

	rc = kstrtouint(instr, 0, &ct);
	if (rc) {
		pr_err("expecting numeric input, using 1 instead\n");
		ct = 1;
	}
	if (ct > PRINT_CLAMP) {
		ct = PRINT_CLAMP;
		pr_info("clamping print-count to %d\n", ct);
	}
	do_prints(ct);
	return 0;
}
static int param_get_do_prints(char *buffer, const struct kernel_param *kp)
{
	do_prints(1);
	return scnprintf(buffer, PAGE_SIZE, "did 1 do_prints\n");
}
static const struct kernel_param_ops param_ops_do_prints = {
	.set = param_set_do_prints,
	.get = param_get_do_prints,
};
module_param_cb(do_prints, &param_ops_do_prints, NULL, 0600);

#define CLASSMAP_BITMASK(width, base) (((1UL << (width)) - 1) << (base))

/* sysfs param wrapper, proto-API */
#define DYNAMIC_DEBUG_CLASSMAP_PARAM_(_model, _flags, _init)		\
	static unsigned long bits_##_model = _init;			\
	static struct _ddebug_class_param _flags##_##_model = {		\
		.bits = &bits_##_model,					\
		.flags = #_flags,					\
		.map = &map_##_model,					\
	};								\
	module_param_cb(_flags##_##_model, &param_ops_dyndbg_classes,	\
			&_flags##_##_model, 0600)
#ifdef DEBUG
#define DYNAMIC_DEBUG_CLASSMAP_PARAM(_model, _flags)		\
	DYNAMIC_DEBUG_CLASSMAP_PARAM_(_model, _flags, ~0)
#else
#define DYNAMIC_DEBUG_CLASSMAP_PARAM(_model, _flags)		\
	DYNAMIC_DEBUG_CLASSMAP_PARAM_(_model, _flags, 0)
#endif

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

/* numeric verbosity, V2 > V1 related.  V0 is > D2_DRMRES */
enum cat_level_num { V0 = 16, V1, V2, V3, V4, V5, V6, V7 };

/* recapitulate DRM's multi-classmap setup */
#if !defined(TEST_DYNAMIC_DEBUG_SUBMOD)
/*
 * In single user, or parent / coordinator (drm.ko) modules, define
 * classmaps on the client enums above, and then declares the PARAMS
 * ref'g the classmaps.  Each is exported.
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
			      V0, "V0", "V1", "V2", "V3", "V4", "V5", "V6", "V7");

/*
 * now add the sysfs-params
 */

DYNAMIC_DEBUG_CLASSMAP_PARAM(disjoint_bits, p);
DYNAMIC_DEBUG_CLASSMAP_PARAM(level_num, p);

#ifdef FORCE_CLASSID_CONFLICT
/*
 * Enable with -Dflag on compile to test overlapping class-id range
 * detection.  This should warn on modprobes.
 */
DYNDBG_CLASSMAP_DEFINE(classid_range_conflict, 0, D2_CORE + 1, "D3_CORE");
#endif

#else /* TEST_DYNAMIC_DEBUG_SUBMOD */

/*
 * in submod/drm-drivers, use the classmaps defined in top/parent
 * module above.
 */

DYNAMIC_DEBUG_CLASSMAP_USE(map_disjoint_bits);
DYNAMIC_DEBUG_CLASSMAP_USE(map_level_num);

#if defined(DD_MACRO_ARGCHECK)
/*
 * Exersize compile-time arg-checks in DYNDBG_CLASSMAP_DEFINE.
 * These will break compilation.
 */
DYNDBG_CLASSMAP_DEFINE(fail_base_neg, 0, -1, "NEGATIVE_BASE_ARG");
DYNDBG_CLASSMAP_DEFINE(fail_base_big, 0, 100, "TOOBIG_BASE_ARG");
DYNDBG_CLASSMAP_DEFINE(fail_str_type, 0, 0, 1 /* not a string */);
DYNDBG_CLASSMAP_DEFINE(fail_emptyclass, 0, 0 /* ,empty */);
#endif

#endif /* TEST_DYNAMIC_DEBUG_SUBMOD */

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

	prdbg(V1);
	prdbg(V2);
	prdbg(V3);
	prdbg(V4);
	prdbg(V5);
	prdbg(V6);
	prdbg(V7);
}

static void do_prints(unsigned int ct)
{
	/* maybe clamp this */
	pr_debug("do-prints %d times:\n", ct);
	for (; ct; ct--) {
		do_cats();
		do_levels();
	}
}

static int __init test_dynamic_debug_init(void)
{
	pr_debug("init start\n");
	do_prints(1);
	pr_debug("init done\n");
	return 0;
}

static void __exit test_dynamic_debug_exit(void)
{
	pr_debug("exited\n");
}

module_init(test_dynamic_debug_init);
module_exit(test_dynamic_debug_exit);

MODULE_AUTHOR("Jim Cromie <jim.cromie@gmail.com>");
MODULE_DESCRIPTION("Kernel module for testing dynamic_debug");
MODULE_LICENSE("GPL");
