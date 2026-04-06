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

struct landlock_domain;
struct landlock_hierarchy;
struct landlock_ruleset;
struct path;

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
 *
 * Network port fields use __u64 in host endianness, matching the
 * landlock_net_port_attr.port UAPI convention.  Callers convert from
 * network byte order before emitting the event.
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

	TP_STRUCT__entry(__field(__u64, ruleset_id) __field(
		__u32, ruleset_version) __field(access_mask_t, handled_fs)
				 __field(access_mask_t, handled_net)
					 __field(access_mask_t, scoped)),

	TP_fast_assign(__entry->ruleset_id = ruleset->id;
		       __entry->ruleset_version = ruleset->version;
		       __entry->handled_fs = ruleset->layer.fs;
		       __entry->handled_net = ruleset->layer.net;
		       __entry->scoped = ruleset->layer.scope;),

	TP_printk("ruleset=%llx.%u handled_fs=0x%x handled_net=0x%x scoped=0x%x",
		  __entry->ruleset_id, __entry->ruleset_version,
		  __entry->handled_fs, __entry->handled_net, __entry->scoped));

/**
 * landlock_free_ruleset - Ruleset freed
 *
 * Emitted when a ruleset's last reference is dropped (typically when
 * the creating process closes the ruleset file descriptor).
 */
TRACE_EVENT(landlock_free_ruleset,

	    TP_PROTO(const struct landlock_ruleset *ruleset),

	    TP_ARGS(ruleset),

	    TP_STRUCT__entry(__field(__u64, ruleset_id)
				     __field(__u32, ruleset_version)),

	    TP_fast_assign(__entry->ruleset_id = ruleset->id;
			   __entry->ruleset_version = ruleset->version;),

	    TP_printk("ruleset=%llx.%u", __entry->ruleset_id,
		      __entry->ruleset_version));

/**
 * landlock_add_rule_fs - filesystem rule added to a ruleset
 * @ruleset: Source ruleset (never NULL)
 * @access_rights: Allowed access mask for this rule
 * @path: Filesystem path for the rule (never NULL)
 * @pathname: Resolved absolute path string (never NULL; error placeholder
 *            on resolution failure)
 */
TRACE_EVENT(
	landlock_add_rule_fs,

	TP_PROTO(const struct landlock_ruleset *ruleset,
		 access_mask_t access_rights, const struct path *path,
		 const char *pathname),

	TP_ARGS(ruleset, access_rights, path, pathname),

	TP_STRUCT__entry(__field(__u64, ruleset_id) __field(__u32,
							    ruleset_version)
				 __field(access_mask_t, access_rights)
					 __field(dev_t, dev) __field(ino_t, ino)
						 __string(pathname, pathname)),

	TP_fast_assign(lockdep_assert_held(&ruleset->lock);
		       __entry->ruleset_id = ruleset->id;
		       __entry->ruleset_version = ruleset->version;
		       __entry->access_rights = access_rights;
		       __entry->dev = path->dentry->d_sb->s_dev;
		       /*
			     * The inode number may not be the user-visible one,
			     * but it will be the same used by audit.
			     */
		       __entry->ino = d_backing_inode(path->dentry)->i_ino;
		       __assign_str(pathname);),

	TP_printk("ruleset=%llx.%u access_rights=0x%x dev=%u:%u ino=%lu path=%s",
		  __entry->ruleset_id, __entry->ruleset_version,
		  __entry->access_rights, MAJOR(__entry->dev),
		  MINOR(__entry->dev), __entry->ino,
		  __print_untrusted_str(pathname)));

/**
 * landlock_add_rule_net - network port rule added to a ruleset
 * @ruleset: Source ruleset (never NULL)
 * @port: Network port number in host endianness
 * @access_rights: Allowed access mask for this rule
 */
TRACE_EVENT(landlock_add_rule_net,

	    TP_PROTO(const struct landlock_ruleset *ruleset, __u64 port,
		     access_mask_t access_rights),

	    TP_ARGS(ruleset, port, access_rights),

	    TP_STRUCT__entry(__field(__u64, ruleset_id) __field(__u32,
								ruleset_version)
				     __field(access_mask_t, access_rights)
					     __field(__u64, port)),

	    TP_fast_assign(lockdep_assert_held(&ruleset->lock);
			   __entry->ruleset_id = ruleset->id;
			   __entry->ruleset_version = ruleset->version;
			   __entry->access_rights = access_rights;
			   __entry->port = port;),

	    TP_printk("ruleset=%llx.%u access_rights=0x%x port=%llu",
		      __entry->ruleset_id, __entry->ruleset_version,
		      __entry->access_rights, __entry->port));

/**
 * landlock_restrict_self - new domain created from landlock_restrict_self()
 * @ruleset: Source ruleset frozen into the domain (never NULL); caller
 *           holds ruleset->lock for BTF consistency.  eBPF programs can
 *           read the full ruleset state via BTF (rules, version, access
 *           masks).
 * @domain: Newly created domain (never NULL, immutable after creation).
 *          eBPF programs can navigate domain->hierarchy->parent for the
 *          parent domain chain.
 *
 * Emitted after the domain is successfully installed (including TSYNC
 * if requested).  The flags-only restrict_self path (ruleset_fd == -1)
 * does not create a domain and does not emit this event.  Restrict_self
 * flags that affect logging (log_same_exec, log_new_exec) are accessible
 * via BTF on domain->hierarchy.
 */
TRACE_EVENT(landlock_restrict_self,

	    TP_PROTO(const struct landlock_ruleset *ruleset,
		     const struct landlock_domain *domain),

	    TP_ARGS(ruleset, domain),

	    TP_STRUCT__entry(__field(__u64, ruleset_id)
				     __field(__u32, ruleset_version)
					     __field(__u64, domain_id)
						     __field(__u64, parent_id)),

	    TP_fast_assign(
		    lockdep_assert_held(&ruleset->lock);
		    __entry->ruleset_id = ruleset->id;
		    __entry->ruleset_version = ruleset->version;
		    __entry->domain_id = domain->hierarchy->id;
		    __entry->parent_id = domain->hierarchy->parent ?
						 domain->hierarchy->parent->id :
						 0;),

	    TP_printk("ruleset=%llx.%u domain=%llx parent=%llx",
		      __entry->ruleset_id, __entry->ruleset_version,
		      __entry->domain_id, __entry->parent_id));

/**
 * landlock_free_domain - domain freed
 * @hierarchy: Hierarchy node being freed (never NULL); eBPF can read
 *             hierarchy->details (creator identity), hierarchy->parent
 *             (domain chain), and hierarchy->log_status via BTF
 *
 * Emitted when the domain's last reference is dropped, either
 * asynchronously from a kworker (via landlock_put_domain_deferred) or
 * synchronously from the calling task (via landlock_put_domain).
 */
TRACE_EVENT(landlock_free_domain,

	    TP_PROTO(const struct landlock_hierarchy *hierarchy),

	    TP_ARGS(hierarchy),

	    TP_STRUCT__entry(__field(__u64, domain_id) __field(__u64, denials)),

	    TP_fast_assign(
		    __entry->domain_id = hierarchy->id;
		    __entry->denials = atomic64_read(&hierarchy->num_denials);),

	    TP_printk("domain=%llx denials=%llu", __entry->domain_id,
		      __entry->denials));

#endif /* _TRACE_LANDLOCK_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
