/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sysctl.h: General linux system control interface
 *
 * Begun 24 March 1995, Stephen Tweedie
 *
 ****************************************************************
 ****************************************************************
 **
 **  WARNING:
 **  The values in this file are exported to user space via 
 **  the sysctl() binary interface.  Do *NOT* change the
 **  numbering of any existing values here, and do not change
 **  any numbers within any one set of values.  If you have to
 **  redefine an existing interface, use a new number for it.
 **  The kernel will then return -ENOTDIR to any application using
 **  the old binary interface.
 **
 ****************************************************************
 ****************************************************************
 */
#ifndef _LINUX_SYSCTL_H
#define _LINUX_SYSCTL_H

#include <linux/list.h>
#include <linux/rcupdate.h>
#include <linux/wait.h>
#include <linux/rbtree.h>
#include <linux/uidgid.h>
#include <uapi/linux/sysctl.h>

/* For the /proc/sys support */
struct completion;
struct ctl_table;
struct nsproxy;
struct ctl_table_root;
struct ctl_table_header;
struct ctl_dir;

/* Keep the same order as in fs/proc/proc_sysctl.c */
#define SYSCTL_ZERO			((void *)&sysctl_vals[0])
#define SYSCTL_ONE			((void *)&sysctl_vals[1])
#define SYSCTL_TWO			((void *)&sysctl_vals[2])
#define SYSCTL_THREE			((void *)&sysctl_vals[3])
#define SYSCTL_FOUR			((void *)&sysctl_vals[4])
#define SYSCTL_ONE_HUNDRED		((void *)&sysctl_vals[5])
#define SYSCTL_TWO_HUNDRED		((void *)&sysctl_vals[6])
#define SYSCTL_ONE_THOUSAND		((void *)&sysctl_vals[7])
#define SYSCTL_THREE_THOUSAND		((void *)&sysctl_vals[8])
#define SYSCTL_INT_MAX			((void *)&sysctl_vals[9])

/* this is needed for the proc_dointvec_minmax for [fs_]overflow UID and GID */
#define SYSCTL_MAXOLDUID		((void *)&sysctl_vals[10])
#define SYSCTL_NEG_ONE			((void *)&sysctl_vals[11])

extern const int sysctl_vals[];

#define SYSCTL_LONG_ZERO	((void *)&sysctl_long_vals[0])
#define SYSCTL_LONG_ONE		((void *)&sysctl_long_vals[1])
#define SYSCTL_LONG_MAX		((void *)&sysctl_long_vals[2])

/**
 *
 * "dir" originates from read_iter (dir = 0) or write_iter (dir = 1)
 * in the file_operations struct at proc/proc_sysctl.c. Its value means
 * one of two things for sysctl:
 * 1. SYSCTL_USER_TO_KERN(dir) Writing to an internal kernel variable from user
 *                             space (dir > 0)
 * 2. SYSCTL_KERN_TO_USER(dir) Writing to a user space buffer from a kernel
 *                             variable (dir == 0).
 */
#define SYSCTL_USER_TO_KERN(dir) (!!(dir))
#define SYSCTL_KERN_TO_USER(dir) (!dir)

extern const unsigned long sysctl_long_vals[];

typedef int proc_handler(const struct ctl_table *ctl, int write, void *buffer,
		size_t *lenp, loff_t *ppos);

int proc_dostring(const struct ctl_table *, int, void *, size_t *, loff_t *);
int proc_dobool(const struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos);

int proc_dointvec(const struct ctl_table *, int, void *, size_t *, loff_t *);
int proc_dointvec_minmax(const struct ctl_table *table, int dir, void *buffer,
			 size_t *lenp, loff_t *ppos);
int proc_dointvec_conv(const struct ctl_table *table, int dir, void *buffer,
		       size_t *lenp, loff_t *ppos,
		       int (*conv)(bool *negp, unsigned long *u_ptr, int *k_ptr,
				   int dir, const struct ctl_table *table));
int proc_int_k2u_conv_kop(ulong *u_ptr, const int *k_ptr, bool *negp,
			  ulong (*k_ptr_op)(const ulong));
int proc_int_u2k_conv_uop(const ulong *u_ptr, int *k_ptr, const bool *negp,
			  ulong (*u_ptr_op)(const ulong));
int proc_int_conv(bool *negp, ulong *u_ptr, int *k_ptr, int dir,
		  const struct ctl_table *tbl, bool k_ptr_range_check,
		  int (*user_to_kern)(const bool *negp, const ulong *u_ptr, int *k_ptr),
		  int (*kern_to_user)(bool *negp, ulong *u_ptr, const int *k_ptr));

int proc_douintvec(const struct ctl_table *, int, void *, size_t *, loff_t *);
int proc_douintvec_minmax(const struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos);
int proc_douintvec_conv(const struct ctl_table *table, int write, void *buffer,
			size_t *lenp, loff_t *ppos,
			int (*conv)(unsigned long *lvalp, unsigned int *valp,
				    int write, const struct ctl_table *table));
int proc_uint_k2u_conv(ulong *u_ptr, const uint *k_ptr);
int proc_uint_u2k_conv_uop(const ulong *u_ptr, uint *k_ptr,
			   ulong (*u_ptr_op)(const ulong));
int proc_uint_conv(ulong *u_ptr, uint *k_ptr, int dir,
		   const struct ctl_table *tbl, bool k_ptr_range_check,
		   int (*user_to_kern)(const ulong *u_ptr, uint *k_ptr),
		   int (*kern_to_user)(ulong *u_ptr, const uint *k_ptr));

int proc_dou8vec_minmax(const struct ctl_table *table, int write, void *buffer,
			size_t *lenp, loff_t *ppos);
int proc_doulongvec_minmax(const struct ctl_table *, int, void *, size_t *, loff_t *);
int proc_doulongvec_minmax_conv(const struct ctl_table *table, int dir,
				void *buffer, size_t *lenp, loff_t *ppos,
				unsigned long convmul, unsigned long convdiv);
int proc_do_large_bitmap(const struct ctl_table *, int, void *, size_t *, loff_t *);
int proc_do_static_key(const struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos);

/*
 * Register a set of sysctl names by calling register_sysctl
 * with an initialised array of struct ctl_table's.
 *
 * sysctl names can be mirrored automatically under /proc/sys.  The
 * procname supplied controls /proc naming.
 *
 * The table's mode will be honoured for proc-fs access.
 *
 * Leaf nodes in the sysctl tree will be represented by a single file
 * under /proc; non-leaf nodes will be represented by directories.  A
 * null procname disables /proc mirroring at this node.
 *
 * The data and maxlen fields of the ctl_table
 * struct enable minimal validation of the values being written to be
 * performed, and the mode field allows minimal authentication.
 * 
 * There must be a proc_handler routine for any terminal nodes
 * mirrored under /proc/sys (non-terminals are handled by a built-in
 * directory handler).  Several default handlers are available to
 * cover common cases.
 */

/* Support for userspace poll() to watch for changes */
struct ctl_table_poll {
	atomic_t event;
	wait_queue_head_t wait;
};

static inline void *proc_sys_poll_event(struct ctl_table_poll *poll)
{
	return (void *)(unsigned long)atomic_read(&poll->event);
}

#define __CTL_TABLE_POLL_INITIALIZER(name) {				\
	.event = ATOMIC_INIT(0),					\
	.wait = __WAIT_QUEUE_HEAD_INITIALIZER(name.wait) }

#define DEFINE_CTL_TABLE_POLL(name)					\
	struct ctl_table_poll name = __CTL_TABLE_POLL_INITIALIZER(name)

/* A sysctl table is an array of struct ctl_table: */
struct ctl_table {
	const char *procname;		/* Text ID for /proc/sys */
	void *data;
	int maxlen;
	umode_t mode;
	proc_handler *proc_handler;	/* Callback for text formatting */
	struct ctl_table_poll *poll;
	void *extra1;
	void *extra2;
} __randomize_layout;

/**
 * struct _sysctl_null_type - sentinel type for variable-less entries
 */
struct _sysctl_null_type { char __dummy; };

/**
 * _sysctl_null_marker - sentinel instance used by SYSCTL_NULL
 *
 * Passed via SYSCTL_NULL to mark entries with no associated variable.
 * Detected by _Generic() dispatch in CTLTBL_ENTRY_XXX().
 */
extern const struct _sysctl_null_type _sysctl_null_marker;

/**
 * SYSCTL_NULL - pass as __var for entries with no associated variable
 */
#define SYSCTL_NULL (_sysctl_null_marker)

/**
 * __CTL_AUTO_HANDLER - select proc_handler based on the variable type
 *
 * Supported types: int, unsigned int, long, unsigned long, bool, u8.
 * char arrays (strings) are not supported; use CTLTBL_ENTRY_VNMH() with
 * proc_dostring instead.
 *
 * @__var: kernel variable (value, not address), or SYSCTL_NULL
 */
#define __CTL_AUTO_HANDLER(__var)					\
	_Generic((__var),						\
			int          : proc_dointvec,			\
			unsigned int : proc_douintvec,			\
			long         : proc_doulongvec_minmax,		\
			unsigned long: proc_doulongvec_minmax,		\
			bool         : proc_dobool,			\
			u8           : proc_dou8vec_minmax,		\
			default      :					\
					0xdeadbeaf)

/**
 * __CTL_AUTO_HANDLER_RANGE - select proc_handler for a range-constrained entry
 *
 * Supported types: int, unsigned int, long, unsigned long, u8.
 * bool is also accepted but proc_dobool ignores extra1/extra2 range bounds.
 *
 * @__var: kernel variable (value, not address), or SYSCTL_NULL
 */
#define __CTL_AUTO_HANDLER_RANGE(__var)					\
	_Generic((__var),						\
			int          : proc_dointvec_minmax,		\
			unsigned int : proc_douintvec_minmax,		\
			long         : proc_doulongvec_minmax,		\
			unsigned long: proc_doulongvec_minmax,		\
			bool         : proc_dobool,			\
			u8           : proc_dou8vec_minmax,		\
			default      :					\
					0xdeadbeaf)

/**
 * __CTL_PROCNAME - validate and return the procname string
 *
 * "" __name "" enforces a string-literal argument at compile time.
 *
 * @__name: procname string literal
 */
#define __CTL_PROCNAME(__name)    ("" __name "")

/**
 * __CTL_DATA - assert __var is addressable; return its address
 *
 * SYSCTL_NULL -> (void *)NULL
 * lvalue __var -> (void *)&(__var)
 *
 * @__var: kernel variable (without &), or SYSCTL_NULL
 */
#define __CTL_DATA(__var)						\
	_Generic((__var),						\
			struct _sysctl_null_type : (void *)NULL,	\
			default :					\
				 (void *)&(__var))

/* Compute maxlen for NULL entries: use explicit MAXLEN if >0, else 0 */
#define __SYSCTL_MAXLEN_NULL(__maxlen)					\
	((__maxlen) > 0 ? (size_t)(__maxlen) : (size_t)0)

/* Compute maxlen: use explicit MAXLEN if >0, else sizeof(VAR) */
#define __SYSCTL_MAXLEN_VAR(__var, __maxlen)				\
	((__maxlen) >= 0 ? (size_t)(__maxlen) : (size_t)sizeof(__var))

/**
 * __CTL_MAXLEN - compute the .maxlen value
 *
 * @__var:    kernel variable (without &), or SYSCTL_NULL
 * @__maxlen: explicit override, or -1 for auto-sizing
 */
#define __CTL_MAXLEN(__var, __maxlen)					\
	_Generic((__var),						\
			struct _sysctl_null_type :			\
				__SYSCTL_MAXLEN_NULL(__maxlen),		\
			default :					\
				__SYSCTL_MAXLEN_VAR(__var, __maxlen)	\
		)

/**
 * __CTL_MODE - validate and return file permission bits
 *
 * @__mode: file permission bits (e.g. 0644)
 */
#define __CTL_MODE(__mode)    (0 ? (umode_t)0 : (__mode))

/**
 * __CTL_HANDLER - validate and return proc_handler
 *
 * @__handler: proc_handler
 */
#define __CTL_HANDLER(__handler) \
	(0 ? (typeof(proc_handler) *)0 : (__handler))

/* Validate PTR is pointer-compatible and return it as void* */
#define __CTL_EXTRA(PTR) \
		(0 ? (void *)0 : (PTR))

/**
 * Internal primitive  (single source of truth)
 *
 * All public macros delegate here.
 *
 * @_var:     kernel variable (without &), or SYSCTL_NULL
 * @_name:    procname string literal
 * @_mode:    file permission bits
 * @_handler: proc_handler-compatible function pointer
 * @_min:     lower bound pointer, or NULL
 * @_max:     upper bound pointer, or NULL
 * @_maxlen:  explicit .maxlen, or -1 for auto-sizing
 */
#define __CTLTBL_ENTRY(_var, _name, _mode, _handler, _min, _max, _maxlen) \
{ \
	.procname     = __CTL_PROCNAME(_name),				\
	.data         = __CTL_DATA(_var),				\
	.maxlen       = __CTL_MAXLEN(_var, _maxlen),			\
	.mode         = __CTL_MODE(_mode),				\
	.proc_handler = __CTL_HANDLER(_handler),			\
	.poll         = NULL,						\
	.extra1       = __CTL_EXTRA(_min),				\
	.extra2       = __CTL_EXTRA(_max),				\
}

/**
 * Public API
 *
 * Naming convention:
 *   V  - Variable is auto-named via #__var
 *   N  - explicit Name string
 *   M  - explicit Mode
 *   H  - explicit Handler
 *   R  - Range checking (min/max, selects _minmax handler)
 *   L  - explicit Length (.maxlen override)
 */

/**
 * CTLTBL_ENTRY_V - read-only entry; procname == stringified variable name
 *
 * Proto: (T __var)
 *
 * .procname = #__var, .mode = 0444, .extra1 = .extra2 = NULL.
 * proc_handler and .maxlen inferred from typeof(__var).
 *
 * @__var: kernel variable (without &); must be an addressable lvalue
 */
#define CTLTBL_ENTRY_V(__var)						\
	__CTLTBL_ENTRY(__var, #__var, 0444,				\
		__CTL_AUTO_HANDLER(__var), NULL, NULL, -1)

/**
 * CTLTBL_ENTRY_VM - entry with custom mode; procname == #__var
 *
 * Proto: (T __var, umode_t __mode)
 *
 * @__var:  kernel variable (without &)
 * @__mode: file permission bits
 */
#define CTLTBL_ENTRY_VM(__var, __mode)					\
	__CTLTBL_ENTRY(__var, #__var, __mode,				\
		__CTL_AUTO_HANDLER(__var), NULL, NULL, -1)

/**
 * CTLTBL_ENTRY_VMR - custom mode with range checking; procname == #__var
 *
 * Proto: (T __var, umode_t __mode, T *__min, T *__max)
 *
 * @__var:  kernel variable (without &)
 * @__mode: file permission bits
 * @__min:  pointer to minimum value; typeof(*__min) must == typeof(__var)
 * @__max:  pointer to maximum value; typeof(*__max) must == typeof(__var)
 */
#define CTLTBL_ENTRY_VMR(__var, __mode, __min, __max)			\
	__CTLTBL_ENTRY(__var, #__var, __mode,				\
		__CTL_AUTO_HANDLER_RANGE(__var), __min, __max, -1)

/**
 * CTLTBL_ENTRY_VN - read-only entry with explicit procname
 *
 * Proto: (T __var, const char *__name)
 *
 * @__var:  kernel variable (without &)
 * @__name: procname string literal
 */
#define CTLTBL_ENTRY_VN(__var, __name)					\
	__CTLTBL_ENTRY(__var, __name, 0444,				\
		__CTL_AUTO_HANDLER(__var), NULL, NULL, -1)

/**
 * CTLTBL_ENTRY_VNM - entry with explicit procname and mode
 *
 * Proto: (T __var, const char *__name, umode_t __mode)
 *
 * @__var:  kernel variable (without &)
 * @__name: procname string literal
 * @__mode: file permission bits
 */
#define CTLTBL_ENTRY_VNM(__var, __name, __mode)				\
	__CTLTBL_ENTRY(__var, __name, __mode,				\
		__CTL_AUTO_HANDLER(__var), NULL, NULL, -1)

/**
 * CTLTBL_ENTRY_VNMH - entry with explicit procname, mode and handler
 *
 * Proto: (T __var, const char *__name, umode_t __mode,
 *         proc_handler *__handler)
 *
 * @__var:     kernel variable (without &), or SYSCTL_NULL
 * @__name:    procname string literal
 * @__mode:    file permission bits
 * @__handler: proc_handler-compatible function pointer
 */
#define CTLTBL_ENTRY_VNMH(__var, __name, __mode, __handler)		\
	__CTLTBL_ENTRY(__var, __name, __mode, __handler, NULL, NULL, -1)

/**
 * CTLTBL_ENTRY_VNMR - entry with procname, mode and range checking
 *
 * Proto: (T __var, const char *__name, umode_t __mode,
 *         T *__min, T *__max)
 *
 * @__var:  kernel variable (without &)
 * @__name: procname string literal
 * @__mode: file permission bits
 * @__min:  pointer to minimum value; typeof(*__min) must == typeof(__var)
 * @__max:  pointer to maximum value; typeof(*__max) must == typeof(__var)
 */
#define CTLTBL_ENTRY_VNMR(__var, __name, __mode, __min, __max)		\
	__CTLTBL_ENTRY(__var, __name, __mode,				\
		__CTL_AUTO_HANDLER_RANGE(__var), __min, __max, -1)

/**
 * CTLTBL_ENTRY_VNMHR - entry with explicit handler and range
 *
 * Proto: (T __var, const char *__name, umode_t __mode,
 *         proc_handler *__handler, T *__min, T *__max)
 *
 * @__var:     kernel variable (without &), or SYSCTL_NULL
 * @__name:    procname string literal
 * @__mode:    file permission bits
 * @__handler: proc_handler-compatible function pointer
 * @__min:     pointer to minimum value for .extra1
 * @__max:     pointer to maximum value for .extra2
 */
#define CTLTBL_ENTRY_VNMHR(__var, __name, __mode, __handler, __min, __max) \
	__CTLTBL_ENTRY(__var, __name, __mode, __handler, __min, __max, -1)

/**
 * CTLTBL_ENTRY_VNMHRL - fully explicit entry
 *
 * Proto: (T __var, const char *__name, umode_t __mode,
 *         proc_handler *__handler, T *__min, T *__max,
 *         size_t __maxlen)
 *
 * Pass -1 for __maxlen to use sizeof(__var) / 0 (auto).
 * Pass  0 to set .maxlen to zero explicitly.
 *
 * @__var:     kernel variable (without &), or SYSCTL_NULL
 * @__name:    procname string literal
 * @__mode:    file permission bits
 * @__handler: proc_handler-compatible function pointer
 * @__min:     pointer to minimum value for .extra1
 * @__max:     pointer to maximum value for .extra2
 * @__maxlen:  explicit value for .maxlen
 */
#define CTLTBL_ENTRY_VNMHRL(__var, __name, __mode, __handler,		\
			    __min, __max, __maxlen)			\
	__CTLTBL_ENTRY(__var, __name, __mode, __handler, __min, __max, __maxlen)

/**
 * CTLTBL_ENTRY_NMH - custom-handler entry with no associated variable
 *
 * Proto: (const char *__name, umode_t __mode,
 *         proc_handler *__handler)
 *
 * Shorthand for CTLTBL_ENTRY_VNMH(SYSCTL_NULL, ...).
 * .data = NULL, .maxlen = 0, .extra1 = NULL, .extra2 = NULL.
 *
 * @__name:    procname string literal
 * @__mode:    file permission bits
 * @__handler: proc_handler-compatible function pointer
 */
#define CTLTBL_ENTRY_NMH(__name, __mode, __handler)			\
	CTLTBL_ENTRY_VNMH(SYSCTL_NULL, __name, __mode, __handler)

struct ctl_node {
	struct rb_node node;
	struct ctl_table_header *header;
};

/**
 * struct ctl_table_header - maintains dynamic lists of struct ctl_table trees
 * @ctl_table: pointer to the first element in ctl_table array
 * @ctl_table_size: number of elements pointed by @ctl_table
 * @used: The entry will never be touched when equal to 0.
 * @count: Upped every time something is added to @inodes and downed every time
 *         something is removed from inodes
 * @nreg: When nreg drops to 0 the ctl_table_header will be unregistered.
 * @rcu: Delays the freeing of the inode. Introduced with "unfuck proc_sysctl ->d_compare()"
 *
 * @type: Enumeration to differentiate between ctl target types
 * @type.SYSCTL_TABLE_TYPE_DEFAULT: ctl target with no special considerations
 * @type.SYSCTL_TABLE_TYPE_PERMANENTLY_EMPTY: Identifies a permanently empty dir
 *                                            target to serve as a mount point
 */
struct ctl_table_header {
	union {
		struct {
			const struct ctl_table *ctl_table;
			int ctl_table_size;
			int used;
			int count;
			int nreg;
		};
		struct rcu_head rcu;
	};
	struct completion *unregistering;
	const struct ctl_table *ctl_table_arg;
	struct ctl_table_root *root;
	struct ctl_table_set *set;
	struct ctl_dir *parent;
	struct ctl_node *node;
	struct hlist_head inodes; /* head for proc_inode->sysctl_inodes */
	enum {
		SYSCTL_TABLE_TYPE_DEFAULT,
		SYSCTL_TABLE_TYPE_PERMANENTLY_EMPTY,
	} type;
};

struct ctl_dir {
	/* Header must be at the start of ctl_dir */
	struct ctl_table_header header;
	struct rb_root root;
};

struct ctl_table_set {
	int (*is_seen)(struct ctl_table_set *);
	struct ctl_dir dir;
};

struct ctl_table_root {
	struct ctl_table_set default_set;
	struct ctl_table_set *(*lookup)(struct ctl_table_root *root);
	void (*set_ownership)(struct ctl_table_header *head,
			      kuid_t *uid, kgid_t *gid);
	int (*permissions)(struct ctl_table_header *head, const struct ctl_table *table);
};

#define register_sysctl(path, table)	\
	register_sysctl_sz(path, table, ARRAY_SIZE(table))

#ifdef CONFIG_SYSCTL

void proc_sys_poll_notify(struct ctl_table_poll *poll);

extern void setup_sysctl_set(struct ctl_table_set *p,
	struct ctl_table_root *root,
	int (*is_seen)(struct ctl_table_set *));
extern void retire_sysctl_set(struct ctl_table_set *set);

struct ctl_table_header *__register_sysctl_table(
	struct ctl_table_set *set,
	const char *path, const struct ctl_table *table, size_t table_size);
struct ctl_table_header *register_sysctl_sz(const char *path, const struct ctl_table *table,
					    size_t table_size);
void unregister_sysctl_table(struct ctl_table_header * table);

extern int sysctl_init_bases(void);
extern void __register_sysctl_init(const char *path, const struct ctl_table *table,
				 const char *table_name, size_t table_size);
#define register_sysctl_init(path, table)	\
	__register_sysctl_init(path, table, #table, ARRAY_SIZE(table))
extern struct ctl_table_header *register_sysctl_mount_point(const char *path);

void do_sysctl_args(void);
bool sysctl_is_alias(char *param);

extern int unaligned_enabled;
extern int no_unaligned_warning;

#else /* CONFIG_SYSCTL */

static inline void register_sysctl_init(const char *path, const struct ctl_table *table)
{
}

static inline struct ctl_table_header *register_sysctl_mount_point(const char *path)
{
	return NULL;
}

static inline struct ctl_table_header *register_sysctl_sz(const char *path,
							  const struct ctl_table *table,
							  size_t table_size)
{
	return NULL;
}

static inline void unregister_sysctl_table(struct ctl_table_header * table)
{
}

static inline void setup_sysctl_set(struct ctl_table_set *p,
	struct ctl_table_root *root,
	int (*is_seen)(struct ctl_table_set *))
{
}

static inline void do_sysctl_args(void)
{
}

static inline bool sysctl_is_alias(char *param)
{
	return false;
}
#endif /* CONFIG_SYSCTL */

#endif /* _LINUX_SYSCTL_H */
