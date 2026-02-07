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

/* Special marker type to represent NULL data in sysctl entries */
struct __sysctl_null_type { char __dummy; };
extern const struct __sysctl_null_type __sysctl_null_marker;

/* Use SYSCTL_NULL to indicate a sysctl entry without associated data */
#define SYSCTL_NULL (__sysctl_null_marker)

/* Validate NAME is a string literal and return it (fails on non-literals) */
#define __SYSCTL_PROCNAME(NAME) \
	("" NAME "")

/* Generate data pointer: NULL for SYSCTL_NULL, &VAR for actual variables */
#define __SYSCTL_DATA(VAR) \
	_Generic((VAR), \
		struct __sysctl_null_type : ((void *)NULL), \
		default : \
			((void *)&(VAR)) \
		)

/* Compute maxlen for NULL entries: use explicit MAXLEN if >0, else 0 */
#define __SYSCTL_MAXLEN_NULL(MAXLEN) \
	((MAXLEN) > 0 ? (size_t)(MAXLEN) : (size_t)0)

/* Compute maxlen: use explicit MAXLEN if >0, else sizeof(VAR) */
#define __SYSCTL_MAXLEN_VAR(VAR, MAXLEN) \
	((MAXLEN) >= 0 ? (size_t)(MAXLEN) : (size_t)sizeof(VAR))

/* Compute maxlen: auto-detect based on entry type (NULL vs variable) */
#define __SYSCTL_MAXLEN(VAR, MAXLEN) \
	_Generic((VAR), \
		struct __sysctl_null_type : __SYSCTL_MAXLEN_NULL(MAXLEN), \
		default : \
			  __SYSCTL_MAXLEN_VAR(VAR, MAXLEN) \
		)

/* Validate MODE is compatible with umode_t and return it */
#define __SYSCTL_MODE(MODE) \
	(0 ? (umode_t)0 : (MODE))

/* Validate HANDLER matches proc_handler signature and return it */
#define __SYSCTL_PROC_HANDLER(HANDLER) \
	(0 ? (proc_handler *)0 : (HANDLER))

/* Auto-select appropriate proc_handler based on VAR's type */
#define __SYSCTL_AUTO_HANDLER(VAR) \
	_Generic((VAR), \
		int : (proc_handler *)proc_dointvec, \
		unsigned int : (proc_handler *)proc_douintvec, \
		long : (proc_handler *)proc_dointvec, \
		unsigned long : (proc_handler *)proc_douintvec, \
		default : \
			(proc_handler *)NULL \
		)

/* Auto-select range-checking proc_handler based on VAR's type */
#define __SYSCTL_AUTO_HANDLER_MINMAX(VAR) \
	_Generic((VAR), \
		int : (proc_handler *)proc_dointvec_minmax, \
		unsigned int : (proc_handler *)proc_douintvec_minmax, \
		long : (proc_handler *)proc_doulongvec_minmax, \
		unsigned long : (proc_handler *)proc_doulongvec_minmax, \
		default : \
			  (proc_handler *)NULL \
		)

/* Validate PTR is pointer-compatible and return it as void* */
#define __SYSCTL_EXTRA(PTR) \
	(0 ? (void *)0 : (PTR))

/* Initialize a ctl_table entry with all parameters */
#define __SYSCTL_TBL_ENTRY(NAME, VAR, MODE, HANDLER, MIN, MAX, MAXLEN) \
	{ \
		.procname     = __SYSCTL_PROCNAME(NAME), \
		.data         = __SYSCTL_DATA(VAR), \
		.maxlen       = __SYSCTL_MAXLEN(VAR, MAXLEN), \
		.mode         = __SYSCTL_MODE(MODE), \
		.proc_handler = __SYSCTL_PROC_HANDLER(HANDLER), \
		.poll         = NULL, \
		.extra1       = __SYSCTL_EXTRA(MIN), \
		.extra2       = __SYSCTL_EXTRA(MAX), \
	}

/* Count the number of variadic arguments (supports 1-7 arguments) */
#define __SYSCTL_NARG(...) \
	__SYSCTL_NARG_(__VA_ARGS__, 7, 6, 5, 4, 3, 2, 1, 0)

/* Helper for __SYSCTL_NARG - maps arguments to their count */
#define __SYSCTL_NARG_(_1, _2, _3, _4, _5, _6, _7, N, ...) N

/* Concatenate two tokens */
#define __SYSCTL_CONCAT(A, B) A##B

/* Select macro variant based on argument count */
#define __SYSCTL_SELECT(NAME, N) __SYSCTL_CONCAT(NAME, N)

/* SYSCTL_TBL_ENTRY(var) - use variable name as procname, readonly, auto handler */
#define __SYSCTL_TBL_ENTRY_1(VAR) \
	__SYSCTL_TBL_ENTRY(#VAR, VAR, 0444, \
			   __SYSCTL_AUTO_HANDLER(VAR), NULL, NULL, -1)

/* SYSCTL_TBL_ENTRY(name, var) - custom name, readonly, auto handler */
#define __SYSCTL_TBL_ENTRY_2(NAME, VAR) \
	__SYSCTL_TBL_ENTRY(NAME, VAR, 0444, \
			   __SYSCTL_AUTO_HANDLER(VAR), NULL, NULL, -1)

/* SYSCTL_TBL_ENTRY(name, var, mode) - custom name and mode, auto handler */
#define __SYSCTL_TBL_ENTRY_3(NAME, VAR, MODE) \
	__SYSCTL_TBL_ENTRY(NAME, VAR, MODE, \
			   __SYSCTL_AUTO_HANDLER(VAR), NULL, NULL, -1)

/* SYSCTL_TBL_ENTRY(name, var, mode, handler) - custom handler */
#define __SYSCTL_TBL_ENTRY_4(NAME, VAR, MODE, HANDLER) \
	__SYSCTL_TBL_ENTRY(NAME, VAR, MODE, HANDLER, NULL, NULL, -1)

/* SYSCTL_TBL_ENTRY(name, var, mode, min, max) - auto range-checking handler */
#define __SYSCTL_TBL_ENTRY_5(NAME, VAR, MODE, MIN, MAX) \
	__SYSCTL_TBL_ENTRY(NAME, VAR, MODE, \
			   __SYSCTL_AUTO_HANDLER_MINMAX(VAR), MIN, MAX, -1)

/* SYSCTL_TBL_ENTRY(name, var, mode, handler, min, max) - custom handler with range */
#define __SYSCTL_TBL_ENTRY_6(NAME, VAR, MODE, HANDLER, MIN, MAX) \
	__SYSCTL_TBL_ENTRY(NAME, VAR, MODE, HANDLER, MIN, MAX, -1)

/* SYSCTL_TBL_ENTRY(name, var, mode, handler, min, max, maxlen) - full control */
#define __SYSCTL_TBL_ENTRY_7(NAME, VAR, MODE, HANDLER, MIN, MAX, MAXLEN) \
	__SYSCTL_TBL_ENTRY(NAME, VAR, MODE, HANDLER, MIN, MAX, MAXLEN)

/* Define a sysctl table entry with automatic type detection and parameter handling */
#define SYSCTL_TBL_ENTRY(...) \
	__SYSCTL_SELECT(__SYSCTL_TBL_ENTRY_, __SYSCTL_NARG(__VA_ARGS__))(__VA_ARGS__)

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
