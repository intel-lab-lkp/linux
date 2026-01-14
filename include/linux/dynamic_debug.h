/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DYNAMIC_DEBUG_H
#define _DYNAMIC_DEBUG_H

#if defined(CONFIG_JUMP_LABEL)
#include <linux/jump_label.h>
#endif

#include <linux/build_bug.h>

/*
 * An instance of this structure is created in a special
 * ELF section at every dynamic debug callsite.  At runtime,
 * the special section is treated as an array of these.
 */
struct _ddebug {
	/*
	 * These fields are used to drive the user interface
	 * for selecting and displaying debug callsites.
	 */
	const char *modname;
	const char *function;
	const char *filename;
	const char *format;
	unsigned int lineno:18;
#define CLS_BITS 6
	unsigned int class_id:CLS_BITS;
#define _DPRINTK_CLASS_DFLT		((1 << CLS_BITS) - 1)
	/*
	 * The flags field controls the behaviour at the callsite.
	 * The bits here are changed dynamically when the user
	 * writes commands to <debugfs>/dynamic_debug/control
	 */
#define _DPRINTK_FLAGS_NONE	0
#define _DPRINTK_FLAGS_PRINT	(1<<0) /* printk() a message using the format */
#define _DPRINTK_FLAGS_INCL_MODNAME	(1<<1)
#define _DPRINTK_FLAGS_INCL_FUNCNAME	(1<<2)
#define _DPRINTK_FLAGS_INCL_LINENO	(1<<3)
#define _DPRINTK_FLAGS_INCL_TID		(1<<4)
#define _DPRINTK_FLAGS_INCL_SOURCENAME	(1<<5)

#define _DPRINTK_FLAGS_INCL_ANY		\
	(_DPRINTK_FLAGS_INCL_MODNAME | _DPRINTK_FLAGS_INCL_FUNCNAME |\
	 _DPRINTK_FLAGS_INCL_LINENO  | _DPRINTK_FLAGS_INCL_TID |\
	 _DPRINTK_FLAGS_INCL_SOURCENAME)

#if defined DEBUG
#define _DPRINTK_FLAGS_DEFAULT _DPRINTK_FLAGS_PRINT
#else
#define _DPRINTK_FLAGS_DEFAULT 0
#endif
	unsigned int flags:8;
#ifdef CONFIG_JUMP_LABEL
	union {
		struct static_key_true dd_key_true;
		struct static_key_false dd_key_false;
	} key;
#endif
} __attribute__((aligned(8)));

enum ddebug_class_map_type {
	DD_CLASS_TYPE_DISJOINT_BITS,
	/**
	 * DD_CLASS_TYPE_DISJOINT_BITS: classes are independent, mapped to bits[0..N].
	 * Expects hex input. Built for drm.debug, basis for other types.
	 */
	DD_CLASS_TYPE_LEVEL_NUM,
	/**
	 * DD_CLASS_TYPE_LEVEL_NUM: input is numeric level, 0..N.
	 * Input N turns on bits 0..N-1
	 */
};

/*
 * map @class_names 0..N to consecutive constants starting at @base.
 */
struct _ddebug_class_map {
	const struct module *mod;	/* NULL for builtins */
	const char *mod_name;
	const char **class_names;
	const int length;
	const int base;		/* index of 1st .class_id, allows split/shared space */
	enum ddebug_class_map_type map_type;
};

struct _ddebug_class_user {
	char *mod_name;
	struct _ddebug_class_map *map;
	const int offset;	/* user offset to re-number the used map */
};

/*
 * @_ddebug_info: gathers module/builtin __dyndbg_<T> __sections
 * together, each is a vec_<T>: a struct { struct T start[], int len }.
 *
 * For builtins, it is used as a cursor, with the inner structs
 * marking sub-vectors of the builtin __sections in DATA_DATA
 */
struct _ddebug_descs {
	struct _ddebug *start;
	int len;
};

struct _ddebug_class_maps {
	struct _ddebug_class_map *start;
	int len;
};

struct _ddebug_class_users {
	struct _ddebug_class_user *start;
	int len;
};

struct _ddebug_info {
	const char *mod_name;
	struct _ddebug_descs descs;
	struct _ddebug_class_maps maps;
	struct _ddebug_class_users users;
};

struct _ddebug_class_param {
	union {
		unsigned long *bits;
		unsigned long *lvl;
	};
	char flags[8];
	const struct _ddebug_class_map *map;
};

/*
 * pr_debug() and friends are globally enabled or modules have selectively
 * enabled them.
 */
#if defined(CONFIG_DYNAMIC_DEBUG) || \
	(defined(CONFIG_DYNAMIC_DEBUG_CORE) && defined(DYNAMIC_DEBUG_MODULE))

/*
 * dyndbg classmaps is modelled closely upon drm.debug:
 *
 *  1. run-time control via sysfs node (api/abi)
 *  2. each bit 0..N controls a single "category"
 *  3. a pr_debug can have only 1 category, not several.
 *  4. "kind" is a compile-time constant: 0..N or BIT() thereof
 *  5. macro impls - give compile-time resolution or fail.
 *
 * dyndbg classmaps design axioms/constraints:
 *
 *  . optimizing compilers use 1-5 above, so preserve them.
 *  . classmaps.class_id *is* the category.
 *  . classmap definers/users are modules.
 *  . every user wants 0..N
 *  . 0..N exposes as ABI
 *  . no 1 use-case wants N > 32, 16 is more usable
 *  . N <= 64 in *all* cases
 *  . modules/subsystems make category/classmap decisions
 *  . ie an enum: DRM has DRM_UT_CORE..DRM_UT_DRMRES
 *  . some categories are exposed to user: ABI
 *  . making modules change their numbering is bogus, avoid if possible
 *
 * We can solve for all these at once:
 *  A: map class-names to a .class_id range at compile-time
 *  B: allow only "class NAME" changes to class'd callsites at run-time
 *  C: users/modules must manage 0..62 hardcoded .class_id range limit.
 *  D: existing pr_debugs get CLASS_DFLT=63
 *
 * By mapping class-names at >control to class-ids underneath, and
 * responding only to class-names DEFINEd or USEd by the module, we
 * can private-ize the class-id, and adjust class'd pr_debugs only by
 * their names.
 *
 * This give us:
 *  E: class_ids without classnames are unreachable
 *  F: user modules opt-in by DEFINEing a classmap and/or USEing another
 *
 * Multi-classmap modules/groups are supported, if the classmaps share
 * the class_id space [0..62] without overlap/conflict.
 *
 * NOTE: Due to the integer class_id, this api cannot disallow these:
 * __pr_debug_cls(0, "fake CORE msg");  works only if a classmap maps 0.
 * __pr_debug_cls(22, "no such class"); compiles but is not reachable
 */

#define __DYNAMIC_DEBUG_CLASSMAP_CHECK(_clnames, _base)			\
	static_assert(((_base) >= 0 && (_base) < _DPRINTK_CLASS_DFLT),	\
		      "_base must be in 0..62");			\
	static_assert(ARRAY_SIZE(_clnames) > 0,				\
		      "classnames array size must be > 0");		\
	static_assert((ARRAY_SIZE(_clnames) + (_base)) < _DPRINTK_CLASS_DFLT, \
		      "_base + classnames.length exceeds range")

/**
 * DYNAMIC_DEBUG_CLASSMAP_DEFINE - define debug classes used by a module.
 * @_var:   name of the classmap, exported for other modules coordinated use.
 * @_mapty: enum ddebug_class_map_type: 0:DISJOINT - independent, 1:LEVEL - v2>v1
 * @_base:  reserve N classids starting at _base, to split 0..62 classid space
 * @classes: names of the N classes.
 *
 * This tells dyndbg what class_ids the module is using: _base..+N, by
 * mapping names onto them.  This qualifies "class NAME" >controls on
 * the defining module, ignoring unknown names.
 */
#define DYNAMIC_DEBUG_CLASSMAP_DEFINE(_var, _mapty, _base, ...)		\
	static const char *_var##_classnames[] = { __VA_ARGS__ };	\
	__DYNAMIC_DEBUG_CLASSMAP_CHECK(_var##_classnames, (_base));	\
	extern struct _ddebug_class_map _var;				\
	struct _ddebug_class_map __aligned(8) __used			\
		__section("__dyndbg_class_maps") _var = {		\
		.mod = THIS_MODULE,					\
		.mod_name = KBUILD_MODNAME,				\
		.base = (_base),					\
		.map_type = (_mapty),					\
		.length = ARRAY_SIZE(_var##_classnames),		\
		.class_names = _var##_classnames,			\
	};								\
	EXPORT_SYMBOL(_var)

/*
 * XXX: keep this until DRM adapts to use the DEFINE/USE api, it
 * differs from DYNAMIC_DEBUG_CLASSMAP_DEFINE by the lack of the
 * extern/EXPORT on the struct init, and cascading thinkos.
 */
#define DECLARE_DYNDBG_CLASSMAP(_var, _maptype, _base, ...)		\
	static const char *_var##_classnames[] = { __VA_ARGS__ };	\
	static struct _ddebug_class_map __aligned(8) __used		\
		__section("__dyndbg_class_maps") _var = {		\
		.mod = THIS_MODULE,					\
		.mod_name = KBUILD_MODNAME,				\
		.base = _base,						\
		.map_type = _maptype,					\
		.length = ARRAY_SIZE(_var##_classnames),		\
		.class_names = _var##_classnames,			\
	}

/**
 * DYNAMIC_DEBUG_CLASSMAP_USE - refer to a classmap, DEFINEd elsewhere.
 * @_var: name of the exported classmap var
 *
 * This tells dyndbg that the module has prdbgs with classids defined
 * in the named classmap.  This qualifies "class NAME" >controls on
 * the user module, and ignores unknown names.
 */
#define DYNAMIC_DEBUG_CLASSMAP_USE(_var)				\
	DYNAMIC_DEBUG_CLASSMAP_USE_(_var, 0, __UNIQUE_ID(_ddebug_class_user))
#define DYNAMIC_DEBUG_CLASSMAP_USE_(_var, _offset, _uname)		\
	extern struct _ddebug_class_map _var;				\
	static struct _ddebug_class_user __aligned(8) __used		\
	__section("__dyndbg_class_users") _uname = {			\
		.mod_name = KBUILD_MODNAME,				\
		.map = &(_var),						\
		.offset = _offset					\
	}

/**
 * DYNAMIC_DEBUG_CLASSMAP_PARAM - control a ddebug-classmap from a sys-param
 * @_name:  sysfs node name
 * @_var:   name of the classmap var defining the controlled classes/bits
 * @_flags: flags to be toggled, typically just 'p'
 *
 * Creates a sysfs-param to control the classes defined by the
 * exported classmap, with bits 0..N-1 mapped to the classes named.
 * This version keeps class-state in a private long int.
 */
#define DYNAMIC_DEBUG_CLASSMAP_PARAM(_name, _var, _flags)		\
	static unsigned long _name##_bvec;				\
	__DYNAMIC_DEBUG_CLASSMAP_PARAM(_name, _name##_bvec, _var, _flags)

/**
 * DYNAMIC_DEBUG_CLASSMAP_PARAM_REF - wrap a classmap with a controlling sys-param
 * @_name:  sysfs node name
 * @_bits:  name of the module's unsigned long bit-vector, ex: __drm_debug
 * @_var:   name of the (exported) classmap var defining the classes/bits
 * @_flags: flags to be toggled, typically just 'p'
 *
 * Creates a sysfs-param to control the classes defined by the
 * exported clasmap, with bits 0..N-1 mapped to the classes named.
 * This version keeps class-state in user @_bits.  This lets drm check
 * __drm_debug elsewhere too.
 */
#define DYNAMIC_DEBUG_CLASSMAP_PARAM_REF(_name, _bits, _var, _flags)	\
	__DYNAMIC_DEBUG_CLASSMAP_PARAM(_name, _bits, _var, _flags)

#define __DYNAMIC_DEBUG_CLASSMAP_PARAM(_name, _bits, _var, _flags)	\
	static struct _ddebug_class_param _name##_##_flags = {		\
		.bits = &(_bits),					\
		.flags = #_flags,					\
		.map = &(_var),						\
	};								\
	module_param_cb(_name, &param_ops_dyndbg_classes,		\
			&_name##_##_flags, 0600)

extern __printf(2, 3)
void __dynamic_pr_debug(struct _ddebug *descriptor, const char *fmt, ...);

struct device;

extern __printf(3, 4)
void __dynamic_dev_dbg(struct _ddebug *descriptor, const struct device *dev,
		       const char *fmt, ...);

struct net_device;

extern __printf(3, 4)
void __dynamic_netdev_dbg(struct _ddebug *descriptor,
			  const struct net_device *dev,
			  const char *fmt, ...);

struct ib_device;

extern __printf(3, 4)
void __dynamic_ibdev_dbg(struct _ddebug *descriptor,
			 const struct ib_device *ibdev,
			 const char *fmt, ...);

#define DEFINE_DYNAMIC_DEBUG_METADATA_CLS(name, cls, fmt)	\
	static struct _ddebug  __aligned(8)			\
	__section("__dyndbg_descriptors") name = {		\
		.modname = KBUILD_MODNAME,			\
		.function = __func__,				\
		.filename = __FILE__,				\
		.format = (fmt),				\
		.lineno = __LINE__,				\
		.flags = _DPRINTK_FLAGS_DEFAULT,		\
		.class_id = cls,				\
		_DPRINTK_KEY_INIT				\
	};							\
	BUILD_BUG_ON_MSG(cls > _DPRINTK_CLASS_DFLT,		\
			 "classid value overflow")

#define DEFINE_DYNAMIC_DEBUG_METADATA(name, fmt)		\
	DEFINE_DYNAMIC_DEBUG_METADATA_CLS(name, _DPRINTK_CLASS_DFLT, fmt)

#ifdef CONFIG_JUMP_LABEL

#ifdef DEBUG

#define _DPRINTK_KEY_INIT .key.dd_key_true = (STATIC_KEY_TRUE_INIT)

#define DYNAMIC_DEBUG_BRANCH(descriptor) \
	static_branch_likely(&descriptor.key.dd_key_true)
#else
#define _DPRINTK_KEY_INIT .key.dd_key_false = (STATIC_KEY_FALSE_INIT)

#define DYNAMIC_DEBUG_BRANCH(descriptor) \
	static_branch_unlikely(&descriptor.key.dd_key_false)
#endif

#else /* !CONFIG_JUMP_LABEL */

#define _DPRINTK_KEY_INIT

#ifdef DEBUG
#define DYNAMIC_DEBUG_BRANCH(descriptor) \
	likely(descriptor.flags & _DPRINTK_FLAGS_PRINT)
#else
#define DYNAMIC_DEBUG_BRANCH(descriptor) \
	unlikely(descriptor.flags & _DPRINTK_FLAGS_PRINT)
#endif

#endif /* CONFIG_JUMP_LABEL */

/*
 * Factory macros: ($prefix)dynamic_func_call($suffix)
 *
 * Lower layer (with __ prefix) gets the callsite metadata, and wraps
 * the func inside a debug-branch/static-key construct.  Upper layer
 * (with _ prefix) does the UNIQUE_ID once, so that lower can ref the
 * name/label multiple times, and tie the elements together.
 * Multiple flavors:
 * (|_cls):	adds in _DPRINT_CLASS_DFLT as needed
 * (|_no_desc):	former gets callsite descriptor as 1st arg (for prdbgs)
 */
#define __dynamic_func_call_cls(id, cls, fmt, func, ...) ({	\
	DEFINE_DYNAMIC_DEBUG_METADATA_CLS(id, cls, fmt);	\
	if (DYNAMIC_DEBUG_BRANCH(id))				\
		func(&id, ##__VA_ARGS__);			\
})
#define __dynamic_func_call(id, fmt, func, ...)				\
	__dynamic_func_call_cls(id, _DPRINTK_CLASS_DFLT, fmt,		\
				func, ##__VA_ARGS__)

#define __dynamic_func_call_cls_no_desc(id, cls, fmt, func, ...) ({	\
	DEFINE_DYNAMIC_DEBUG_METADATA_CLS(id, cls, fmt);		\
	if (DYNAMIC_DEBUG_BRANCH(id))					\
		func(__VA_ARGS__);					\
})
#define __dynamic_func_call_no_desc(id, fmt, func, ...)			\
	__dynamic_func_call_cls_no_desc(id, _DPRINTK_CLASS_DFLT,	\
					fmt, func, ##__VA_ARGS__)

/*
 * "Factory macro" for generating a call to func, guarded by a
 * DYNAMIC_DEBUG_BRANCH. The dynamic debug descriptor will be
 * initialized using the fmt argument. The function will be called with
 * the address of the descriptor as first argument, followed by all
 * the varargs. Note that fmt is repeated in invocations of this
 * macro.
 */
#define _dynamic_func_call_cls(cls, fmt, func, ...)			\
	__dynamic_func_call_cls(__UNIQUE_ID(_ddebug), cls, fmt, func, ##__VA_ARGS__)
#define _dynamic_func_call(fmt, func, ...)				\
	_dynamic_func_call_cls(_DPRINTK_CLASS_DFLT, fmt, func, ##__VA_ARGS__)

/*
 * A variant that does the same, except that the descriptor is not
 * passed as the first argument to the function; it is only called
 * with precisely the macro's varargs.
 */
#define _dynamic_func_call_cls_no_desc(cls, fmt, func, ...)		\
	__dynamic_func_call_cls_no_desc(__UNIQUE_ID(_ddebug), cls, fmt,	\
					func, ##__VA_ARGS__)
#define _dynamic_func_call_no_desc(fmt, func, ...)			\
	_dynamic_func_call_cls_no_desc(_DPRINTK_CLASS_DFLT, fmt,	\
				       func, ##__VA_ARGS__)

#define dynamic_pr_debug_cls(cls, fmt, ...)				\
	_dynamic_func_call_cls(cls, fmt, __dynamic_pr_debug,		\
			   pr_fmt(fmt), ##__VA_ARGS__)

#define dynamic_pr_debug(fmt, ...)				\
	_dynamic_func_call(fmt, __dynamic_pr_debug,		\
			   pr_fmt(fmt), ##__VA_ARGS__)

#define dynamic_dev_dbg(dev, fmt, ...)				\
	_dynamic_func_call(fmt, __dynamic_dev_dbg, 		\
			   dev, fmt, ##__VA_ARGS__)

#define dynamic_netdev_dbg(dev, fmt, ...)			\
	_dynamic_func_call(fmt, __dynamic_netdev_dbg,		\
			   dev, fmt, ##__VA_ARGS__)

#define dynamic_ibdev_dbg(dev, fmt, ...)			\
	_dynamic_func_call(fmt, __dynamic_ibdev_dbg,		\
			   dev, fmt, ##__VA_ARGS__)

#define dynamic_hex_dump(prefix_str, prefix_type, rowsize,		\
			 groupsize, buf, len, ascii)			\
	_dynamic_func_call_no_desc(__builtin_constant_p(prefix_str) ? prefix_str : "hexdump", \
				   print_hex_dump,			\
				   KERN_DEBUG, prefix_str, prefix_type,	\
				   rowsize, groupsize, buf, len, ascii)

/*
 * This is the "model" class variant of pr_debug.  It is not really
 * intended for direct use; I'd encourage DRM-style drm_dbg_<T>
 * macros for the interface, along with an enum for the <T>
 *
 * __printf(2, 3) would apply.
 */
#define __pr_debug_cls(cls, fmt, ...) ({			\
	BUILD_BUG_ON_MSG(!__builtin_constant_p(cls),		\
			 "expecting constant class int/enum");	\
	dynamic_pr_debug_cls(cls, fmt, ##__VA_ARGS__);		\
})

#else /* !(CONFIG_DYNAMIC_DEBUG || (CONFIG_DYNAMIC_DEBUG_CORE && DYNAMIC_DEBUG_MODULE)) */

#include <linux/string.h>
#include <linux/errno.h>
#include <linux/printk.h>

#define DYNAMIC_DEBUG_CLASSMAP_DEFINE(_var, _mapty, _base, ...)
#define DYNAMIC_DEBUG_CLASSMAP_USE(_var)
#define DYNAMIC_DEBUG_CLASSMAP_PARAM(_name, _var, _flags)
#define DYNAMIC_DEBUG_CLASSMAP_PARAM_REF(_name, _var, _flags)
#define DEFINE_DYNAMIC_DEBUG_METADATA(name, fmt)
#define DYNAMIC_DEBUG_BRANCH(descriptor) false
#define DECLARE_DYNDBG_CLASSMAP(...)

#define dynamic_pr_debug(fmt, ...)					\
	no_printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#define dynamic_dev_dbg(dev, fmt, ...)					\
	dev_no_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__)
#define dynamic_hex_dump(prefix_str, prefix_type, rowsize,		\
			 groupsize, buf, len, ascii)			\
	do { if (0)							\
		print_hex_dump(KERN_DEBUG, prefix_str, prefix_type,	\
				rowsize, groupsize, buf, len, ascii);	\
	} while (0)

#endif /* CONFIG_DYNAMIC_DEBUG || (CONFIG_DYNAMIC_DEBUG_CORE && DYNAMIC_DEBUG_MODULE) */


#ifdef CONFIG_DYNAMIC_DEBUG_CORE

extern int ddebug_dyndbg_module_param_cb(char *param, char *val,
					const char *modname);
struct kernel_param;
int param_set_dyndbg_classes(const char *instr, const struct kernel_param *kp);
int param_get_dyndbg_classes(char *buffer, const struct kernel_param *kp);

#else

static inline int ddebug_dyndbg_module_param_cb(char *param, char *val,
						const char *modname)
{
	if (!strcmp(param, "dyndbg")) {
		/* avoid pr_warn(), which wants pr_fmt() fully defined */
		printk(KERN_WARNING "dyndbg param is supported only in "
			"CONFIG_DYNAMIC_DEBUG builds\n");
		return 0; /* allow and ignore */
	}
	return -EINVAL;
}

struct kernel_param;
static inline int param_set_dyndbg_classes(const char *instr, const struct kernel_param *kp)
{ return 0; }
static inline int param_get_dyndbg_classes(char *buffer, const struct kernel_param *kp)
{ return 0; }

#endif /* !CONFIG_DYNAMIC_DEBUG_CORE */

extern const struct kernel_param_ops param_ops_dyndbg_classes;

#endif /* _DYNAMIC_DEBUG_H */
