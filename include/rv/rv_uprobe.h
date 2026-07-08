/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Generic uprobe infrastructure for RV monitors.
 *
 */

#ifndef _RV_UPROBE_H
#define _RV_UPROBE_H

#include <linux/types.h>
#include <linux/uprobes.h>

struct pt_regs;
struct inode;

/**
 * struct rv_uprobe - embeddable uprobe handle for RV monitors
 *
 * Embed via DECLARE_RV_UPROBE() in the caller's struct and pass &name to
 * rv_uprobe_register().
 *
 * Lifetime: after rv_uprobe_unregister() (or rv_uprobe_unregister_nosync()
 * followed by rv_uprobe_sync()) returns, synchronize_rcu_tasks_trace() has
 * completed and no handler_chain() iteration can reference this struct.
 * The caller may free the containing struct immediately after.
 *
 * @uc:     embedded uprobe_consumer; set uc.handler / uc.ret_handler before
 *          calling rv_uprobe_register(); use container_of(uc, rv_uprobe, uc)
 *          inside handlers to reach this struct or its container
 * @uprobe: registered uprobe pointer (NULL when not registered)
 * @inode:  inode of the probed binary (valid while registered)
 */
struct rv_uprobe {
	struct uprobe_consumer	uc;
	struct uprobe		*uprobe;
	struct inode		*inode;
};

/* Embed a named rv_uprobe inside a caller struct */
#define DECLARE_RV_UPROBE(name)		struct rv_uprobe name

/**
 * rv_uprobe_is_registered - test whether an uprobe is currently active
 * @p: probe to test; may be NULL
 */
bool rv_uprobe_is_registered(const struct rv_uprobe *p);

/**
 * rv_uprobe_register - initialise and register an uprobe
 * @binpath: absolute path to the target binary
 * @offset:  byte offset within the binary
 * @p:       caller-provided rv_uprobe (embedded via DECLARE_RV_UPROBE);
 *           p->uc.handler and/or p->uc.ret_handler must be set before this call
 *
 * Resolves the path and registers p->uc with the uprobe subsystem.
 * No heap allocation is performed.
 *
 * Returns 0 on success, negative errno on failure.
 */
int rv_uprobe_register(const char *binpath, loff_t offset, struct rv_uprobe *p);

/**
 * rv_uprobe_unregister - synchronously unregister a uprobe
 * @p: probe to unregister; may be NULL (no-op)
 *
 * Removes the consumer from the uprobe subsystem and waits for all in-flight
 * handlers to complete (via synchronize_rcu_tasks_trace()).  After this
 * returns, the containing struct may be safely freed by the caller.
 * Use rv_uprobe_unregister_nosync() + rv_uprobe_sync() to batch multiple
 * deregistrations before a single synchronisation.
 */
void rv_uprobe_unregister(struct rv_uprobe *p);

/**
 * rv_uprobe_unregister_nosync - dequeue an uprobe without waiting
 * @p: probe to dequeue; may be NULL (no-op)
 *
 * Removes the consumer without waiting for in-flight handlers.  The caller
 * must call rv_uprobe_sync() before freeing the containing struct.
 */
void rv_uprobe_unregister_nosync(struct rv_uprobe *p);

/**
 * rv_uprobe_sync - wait for all in-flight uprobe handlers to complete
 *
 * Global barrier: calls uprobe_unregister_sync() which performs
 * synchronize_rcu_tasks_trace() + synchronize_srcu(&uretprobes_srcu).
 * After this returns, no handler_chain() iteration referencing any
 * previously deregistered consumer is still in progress.
 */
void rv_uprobe_sync(void);

#endif /* _RV_UPROBE_H */
