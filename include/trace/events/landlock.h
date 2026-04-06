/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright © 2025 Microsoft Corporation
 * Copyright © 2026 Cloudflare
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM landlock

#if !defined(_TRACE_LANDLOCK_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_LANDLOCK_H

#include <linux/tracepoint.h>

struct landlock_ruleset;

/**
 * DOC: Landlock trace events
 *
 * Consistency guarantee: every trace event corresponds to an operation
 * that has irrevocably succeeded.  Lifecycle events fire only after
 * the point of no return; denial events fire only for denials that
 * actually happen.  This guarantees that eBPF programs observing the
 * trace stream can build a faithful model of Landlock state without
 * reconciliation logic.
 *
 * Mutable object pointers in TP_PROTO (e.g., struct landlock_ruleset
 * for add_rule events) are passed while the caller holds the object's
 * lock, so that TP_fast_assign and eBPF programs reading via BTF see a
 * consistent snapshot.  For objects that are immutable at the emission
 * site (e.g., a domain after creation), no lock is needed.
 *
 * All pointer arguments in TP_PROTO are guaranteed non-NULL by the
 * caller.  eBPF programs can access these pointers via BTF for richer
 * introspection than the TP_STRUCT__entry fields provide.
 *
 * TP_STRUCT__entry fields serve TP_printk display only.  eBPF programs
 * access the raw TP_PROTO arguments directly.
 *
 * Security: as for audit, Landlock trace events may expose sensitive
 * information about all sandboxed processes on the system.  See
 * Documentation/admin-guide/LSM/landlock.rst for security considerations
 * and privilege requirements.
 */

/**
 * landlock_create_ruleset - new ruleset created
 * @ruleset: Newly created ruleset (never NULL); not yet shared via an fd,
 *           so no lock is needed.  eBPF programs can read the full ruleset
 *           state via BTF.
 */
TRACE_EVENT(
	landlock_create_ruleset,

	TP_PROTO(const struct landlock_ruleset *ruleset),

	TP_ARGS(ruleset),

	TP_STRUCT__entry(__field(__u64, ruleset_id) __field(access_mask_t,
							    handled_fs)
				 __field(access_mask_t, handled_net)
					 __field(access_mask_t, scoped)),

	TP_fast_assign(__entry->ruleset_id = ruleset->id;
		       __entry->handled_fs = ruleset->layer.fs;
		       __entry->handled_net = ruleset->layer.net;
		       __entry->scoped = ruleset->layer.scope;),

	TP_printk("ruleset=%llx handled_fs=0x%x handled_net=0x%x scoped=0x%x",
		  __entry->ruleset_id, __entry->handled_fs,
		  __entry->handled_net, __entry->scoped));

/**
 * landlock_free_ruleset - Ruleset freed
 *
 * Emitted when a ruleset's last reference is dropped (typically when
 * the creating process closes the ruleset file descriptor).
 */
TRACE_EVENT(landlock_free_ruleset,

	    TP_PROTO(const struct landlock_ruleset *ruleset),

	    TP_ARGS(ruleset),

	    TP_STRUCT__entry(__field(__u64, ruleset_id)),

	    TP_fast_assign(__entry->ruleset_id = ruleset->id;),

	    TP_printk("ruleset=%llx", __entry->ruleset_id));

#endif /* _TRACE_LANDLOCK_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
