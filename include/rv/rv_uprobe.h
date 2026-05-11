/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Generic uprobe infrastructure for RV monitors.
 *
 */

#ifndef _RV_UPROBE_H
#define _RV_UPROBE_H

#include <linux/path.h>
#include <linux/types.h>

struct pt_regs;

/**
 * struct rv_uprobe - a single uprobe registered on behalf of an RV monitor
 *
 * @offset:   byte offset within the ELF binary where the probe is installed
 * @priv:     monitor-private pointer; set at attach time, never touched by
 *            this layer; passed unchanged to entry_fn / ret_fn
 * @path:     resolved path of the probed binary (read-only after attach);
 *            callers may use path.dentry for identity comparisons
 *
 * The implementation fields (uprobe_consumer, uprobe handle, callbacks) are
 * private to rv_uprobe.c and are not exposed here; monitors must not access
 * them directly.
 */
struct rv_uprobe {
	/* public: read-only after rv_uprobe_attach*() */
	loff_t		 offset;
	void		*priv;
	struct path	 path;
};

/**
 * rv_uprobe_attach_path - register an uprobe given an already-resolved path
 * @path:     path of the target binary; rv_uprobe takes its own reference
 * @offset:   byte offset within the binary
 * @entry_fn: called on probe hit (entry); may be NULL
 * @ret_fn:   called on function return (uretprobe); may be NULL
 * @priv:     opaque pointer forwarded to callbacks unchanged
 *
 * Use this variant when the caller has already resolved the path (e.g. to
 * register multiple probes on the same binary with a single kern_path call).
 * The inode is derived internally via d_real_inode(), so inode and path are
 * always consistent.
 *
 * Returns a pointer to the new rv_uprobe on success, ERR_PTR on failure.
 */
struct rv_uprobe *rv_uprobe_attach_path(struct path *path, loff_t offset,
	int (*entry_fn)(struct rv_uprobe *p, struct pt_regs *regs, __u64 *data),
	int (*ret_fn)(struct rv_uprobe *p, unsigned long func,
			struct pt_regs *regs, __u64 *data),
	void *priv);

/**
 * rv_uprobe_attach - resolve binpath and register an uprobe
 * @binpath:  absolute path to the target binary
 * @offset:   byte offset within the binary
 * @entry_fn: called on probe hit (entry); may be NULL
 * @ret_fn:   called on function return (uretprobe); may be NULL
 * @priv:     opaque pointer forwarded to callbacks unchanged
 *
 * Resolves binpath via kern_path(), then delegates to rv_uprobe_attach_path().
 *
 * Returns a pointer to the new rv_uprobe on success, ERR_PTR on failure.
 */
struct rv_uprobe *rv_uprobe_attach(const char *binpath, loff_t offset,
	int (*entry_fn)(struct rv_uprobe *p, struct pt_regs *regs, __u64 *data),
	int (*ret_fn)(struct rv_uprobe *p, unsigned long func,
			struct pt_regs *regs, __u64 *data),
	void *priv);

/**
 * rv_uprobe_detach - synchronously unregister an uprobe and free it
 * @p:  probe to detach; may be NULL (no-op)
 *
 * Calls uprobe_unregister_nosync(), then uprobe_unregister_sync() to wait
 * for any in-progress handler to finish, then releases the path reference
 * and frees the rv_uprobe struct.  The caller's priv data is NOT freed.
 *
 * When removing a single probe, prefer this over the three-phase API.
 * Safe to call from process context only (uprobe_unregister_sync() may
 * schedule).
 */
void rv_uprobe_detach(struct rv_uprobe *p);

/**
 * rv_uprobe_unregister_nosync - dequeue an uprobe without waiting
 * @p:  probe to dequeue; may be NULL (no-op)
 *
 * Removes the uprobe from the uprobe subsystem but does NOT wait for
 * in-flight handlers to complete.  The caller must call rv_uprobe_sync()
 * before calling rv_uprobe_free() on the same probe.
 *
 * Use this to batch multiple deregistrations before a single rv_uprobe_sync().
 */
void rv_uprobe_unregister_nosync(struct rv_uprobe *p);

/**
 * rv_uprobe_sync - wait for all in-flight uprobe handlers to complete
 *
 * Global barrier: waits for every in-flight uprobe handler across the system
 * to finish.  Call once after a batch of rv_uprobe_unregister_nosync() calls
 * and before any rv_uprobe_free() call.
 */
void rv_uprobe_sync(void);

/**
 * rv_uprobe_free - release resources of a previously deregistered probe
 * @p:  probe to free; may be NULL (no-op)
 *
 * Releases the path reference and frees the rv_uprobe struct.  Must only
 * be called after rv_uprobe_sync() has returned.  The caller's priv data
 * is NOT freed.
 */
void rv_uprobe_free(struct rv_uprobe *p);

#endif /* _RV_UPROBE_H */
