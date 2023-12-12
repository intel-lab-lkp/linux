/* Keyrings tracepoints
 *
 * Copyright (C) 2024 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM key

#if !defined(_TRACE_KEY_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_KEY_H

#include <linux/tracepoint.h>

struct key;

/*
 * Declare tracing information enums and their string mappings for display.
 */
#define key_ref_traces \
	EM(key_trace_ref_alloc,			"ALLOC") \
	EM(key_trace_ref_free,			"FREE ") \
	EM(key_trace_ref_gc,			"GC   ") \
	EM(key_trace_ref_get,			"GET  ") \
	EM(key_trace_ref_put,			"PUT  ") \
	E_(key_trace_ref_try_get,		"GET  ")

#define key_dead_traces \
	EM(key_trace_dead_type_removed,		"TYPR") \
	EM(key_trace_dead_domain_removed,	"DOMR") \
	EM(key_trace_dead_expired,		"EXPD") \
	E_(key_trace_dead_invalidated,		"INVL")

/*
 * Generate enums for tracing information.
 */
#ifndef __NETFS_DECLARE_TRACE_ENUMS_ONCE_ONLY
#define __NETFS_DECLARE_TRACE_ENUMS_ONCE_ONLY

#undef EM
#undef E_
#define EM(a, b) a,
#define E_(a, b) a

enum key_dead_trace	{ key_dead_traces } __mode(byte);
enum key_ref_trace	{ key_ref_traces } __mode(byte);

#endif /* end __RXRPC_DECLARE_TRACE_ENUMS_ONCE_ONLY */

/*
 * Export enum symbols via userspace.
 */
#undef EM
#undef E_
#define EM(a, b) TRACE_DEFINE_ENUM(a);
#define E_(a, b) TRACE_DEFINE_ENUM(a);

key_dead_traces;
key_ref_traces;

/*
 * Now redefine the EM() and E_() macros to map the enums to the strings that
 * will be printed in the output.
 */
#undef EM
#undef E_
#define EM(a, b)	{ a, b },
#define E_(a, b)	{ a, b }

TRACE_EVENT(key_alloc,
	    TP_PROTO(const struct key *key),

	    TP_ARGS(key),

	    TP_STRUCT__entry(
		    __field(key_serial_t,		key)
		    __field(uid_t,			uid)
		    __array(char,			type, 8)
		    __array(char,			desc, 24)
			     ),

	    TP_fast_assign(
		    __entry->key = key->serial;
		    __entry->uid = from_kuid(&init_user_ns, key->uid);
		    strncpy(__entry->type, key->type->name, sizeof(__entry->type) - 1);
		    strncpy(__entry->desc, key->description ?: "", sizeof(__entry->desc) - 1);
		    __entry->type[sizeof(__entry->type) - 1] = 0;
		    __entry->desc[sizeof(__entry->desc) - 1] = 0;
			   ),

	    TP_printk("key=%08x uid=%08x t=%s d=%s",
		      __entry->key,
		      __entry->uid,
		      __entry->type,
		      __entry->desc)
	    );

TRACE_EVENT(key_ref,
	    TP_PROTO(key_serial_t key, unsigned int ref, enum key_ref_trace trace,
		     const void *where),

	    TP_ARGS(key, ref, trace, where),

	    TP_STRUCT__entry(
		    __field(key_serial_t,		key)
		    __field(enum key_ref_trace,		trace)
		    __field(unsigned int,		ref)
		    __field(const void *,		where)
			     ),

	    TP_fast_assign(
		    __entry->key = key;
		    __entry->trace = trace;
		    __entry->ref = ref;
		    __entry->where = where;
			   ),

	    TP_printk("key=%08x %s r=%d pc=%pSR",
		      __entry->key,
		      __print_symbolic(__entry->trace, key_ref_traces),
		      __entry->ref,
		      __entry->where)
	    );

TRACE_EVENT(key_instantiate,
	    TP_PROTO(const struct key *key, const struct key_preparsed_payload *prep),

	    TP_ARGS(key, prep),

	    TP_STRUCT__entry(
		    __field(key_serial_t,	key)
		    __field(unsigned int,	datalen)
		    __field(unsigned int,	quotalen)
			     ),

	    TP_fast_assign(
		    __entry->key = key->serial;
		    __entry->datalen = prep->datalen;
		    __entry->quotalen = prep->quotalen;
			   ),

	    TP_printk("key=%08x dlen=%u qlen=%u",
		      __entry->key,
		      __entry->datalen,
		      __entry->quotalen)
	    );

TRACE_EVENT(key_reject,
	    TP_PROTO(const struct key *key, int error),

	    TP_ARGS(key, error),

	    TP_STRUCT__entry(
		    __field(key_serial_t,	key)
		    __field(int,		error)
			     ),

	    TP_fast_assign(
		    __entry->key = key->serial;
		    __entry->error = error;
			   ),

	    TP_printk("key=%08x err=%d",
		      __entry->key,
		      __entry->error)
	    );

TRACE_EVENT(key_update,
	    TP_PROTO(const struct key *key, const struct key_preparsed_payload *prep),

	    TP_ARGS(key, prep),

	    TP_STRUCT__entry(
		    __field(key_serial_t,	key)
		    __field(unsigned int,	datalen)
		    __field(unsigned int,	quotalen)
			     ),

	    TP_fast_assign(
		    __entry->key = key->serial;
		    __entry->datalen = prep->datalen;
		    __entry->quotalen = prep->quotalen;
			   ),

	    TP_printk("key=%08x dlen=%u qlen=%u",
		      __entry->key,
		      __entry->datalen,
		      __entry->quotalen)
	    );

TRACE_EVENT(key_dead,
	    TP_PROTO(const struct key *key, enum key_dead_trace trace),

	    TP_ARGS(key, trace),

	    TP_STRUCT__entry(
		    __field(key_serial_t,		key)
		    __field(enum key_dead_trace,	trace)
			     ),

	    TP_fast_assign(
		    __entry->key = key->serial;
		    __entry->trace = trace;
			   ),

	    TP_printk("key=%08x %s",
		      __entry->key,
		      __print_symbolic(__entry->trace, key_dead_traces))
	    );

TRACE_EVENT(key_quota,
	    TP_PROTO(const struct key_user *user, int change_keys, int change_bytes),

	    TP_ARGS(user, change_keys, change_bytes),

	    TP_STRUCT__entry(
		    __field(uid_t,		uid)
		    __field(unsigned int,	nkeys)
		    __field(unsigned int,	nikeys)
		    __field(unsigned int,	qnkeys)
		    __field(unsigned int,	qnbytes)
		    __field(int,		change_keys)
		    __field(int,		change_bytes)
			     ),

	    TP_fast_assign(
		    __entry->uid = from_kuid(&init_user_ns, user->uid);
		    __entry->nkeys = atomic_read(&user->nkeys);
		    __entry->nikeys = atomic_read(&user->nikeys);
		    __entry->qnkeys = user->qnkeys;
		    __entry->qnbytes = user->qnbytes;
		    __entry->change_keys = change_keys;
		    __entry->change_bytes = change_bytes;
			   ),

	    TP_printk("uid=%d nkeys=%u/%u qkeys=%u qpay=%u ckeys=%d cpay=%d",
		      __entry->uid,
		      __entry->nikeys, __entry->nkeys,
		      __entry->qnkeys,
		      __entry->qnbytes,
		      __entry->change_keys, __entry->change_bytes)
	    );

TRACE_EVENT(key_edquot,
	    TP_PROTO(const struct key_user *user, unsigned int maxkeys,
		     unsigned int maxbytes, unsigned int reqbytes),

	    TP_ARGS(user, maxkeys, maxbytes, reqbytes),

	    TP_STRUCT__entry(
		    __field(uid_t,		uid)
		    __field(unsigned int,	nkeys)
		    __field(unsigned int,	nikeys)
		    __field(unsigned int,	qnkeys)
		    __field(unsigned int,	qnbytes)
		    __field(unsigned int,	maxkeys)
		    __field(unsigned int,	maxbytes)
		    __field(unsigned int,	reqbytes)
			     ),

	    TP_fast_assign(
		    __entry->uid = from_kuid(&init_user_ns, user->uid);
		    __entry->nkeys = atomic_read(&user->nkeys);
		    __entry->nikeys = atomic_read(&user->nikeys);
		    __entry->qnkeys = user->qnkeys;
		    __entry->qnbytes = user->qnbytes;
		    __entry->maxkeys = maxkeys;
		    __entry->maxbytes = maxbytes;
		    __entry->reqbytes = reqbytes;
			   ),

	    TP_printk("u=%d nkeys=%u/%u qkeys=%u/%u qpay=%u/%u cpay=%u",
		      __entry->uid,
		      __entry->nikeys, __entry->nkeys,
		      __entry->qnkeys, __entry->maxkeys,
		      __entry->qnbytes, __entry->maxbytes,
		      __entry->reqbytes)
	    );

TRACE_EVENT(key_link,
	    TP_PROTO(const struct key *keyring, const struct key *key),

	    TP_ARGS(keyring, key),

	    TP_STRUCT__entry(
		    __field(key_serial_t,	keyring)
		    __field(key_serial_t,	key)
			     ),

	    TP_fast_assign(
		    __entry->keyring = keyring->serial;
		    __entry->key = key->serial;
			   ),

	    TP_printk("key=%08x to=%08x",
		      __entry->key, __entry->keyring)
	    );

TRACE_EVENT(key_unlink,
	    TP_PROTO(const struct key *keyring, const struct key *key),

	    TP_ARGS(keyring, key),

	    TP_STRUCT__entry(
		    __field(key_serial_t,	keyring)
		    __field(key_serial_t,	key)
			     ),

	    TP_fast_assign(
		    __entry->keyring = keyring->serial;
		    __entry->key = key->serial;
			   ),

	    TP_printk("key=%08x from=%08x",
		      __entry->key, __entry->keyring)
	    );

TRACE_EVENT(key_move,
	    TP_PROTO(const struct key *from, const struct key *to,
		     const struct key *key),

	    TP_ARGS(from, to, key),

	    TP_STRUCT__entry(
		    __field(key_serial_t,	from)
		    __field(key_serial_t,	to)
		    __field(key_serial_t,	key)
			     ),

	    TP_fast_assign(
		    __entry->from = from->serial;
		    __entry->to = to->serial;
		    __entry->key = key->serial;
			   ),

	    TP_printk("key=%08x from=%08x to=%08x",
		      __entry->key, __entry->from, __entry->to)
	    );

TRACE_EVENT(key_clear,
	    TP_PROTO(const struct key *keyring),
	    TP_ARGS(keyring),
	    TP_STRUCT__entry(
		    __field(key_serial_t,	keyring)
			     ),
	    TP_fast_assign(
		    __entry->keyring = keyring->serial;
			   ),
	    TP_printk("key=%08x",
		      __entry->keyring)
	    );

TRACE_EVENT(key_revoke,
	    TP_PROTO(const struct key *key),
	    TP_ARGS(key),
	    TP_STRUCT__entry(
		    __field(key_serial_t,	key)
			     ),
	    TP_fast_assign(
		    __entry->key = key->serial;
			   ),
	    TP_printk("key=%08x",
		      __entry->key)
	    );

TRACE_EVENT(key_invalidate,
	    TP_PROTO(const struct key *key),
	    TP_ARGS(key),
	    TP_STRUCT__entry(
		    __field(key_serial_t,	key)
			     ),
	    TP_fast_assign(
		    __entry->key = key->serial;
			   ),
	    TP_printk("key=%08x",
		      __entry->key)
	    );

TRACE_EVENT(key_gc,
	    TP_PROTO(const struct key *key),
	    TP_ARGS(key),
	    TP_STRUCT__entry(
		    __field(key_serial_t,	key)
			     ),
	    TP_fast_assign(
		    __entry->key = key->serial;
			   ),
	    TP_printk("key=%08x",
		      __entry->key)
	    );

#undef EM
#undef E_
#endif /* _TRACE_KEY_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
