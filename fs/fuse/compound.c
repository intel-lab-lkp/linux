// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE: Filesystem in Userspace
 * Copyright (C) 2025-2026
 *
 * Compound operations for FUSE - batch multiple operations into a single
 * request to reduce round trips between kernel and userspace.
 */

#include "fuse_i.h"

/*
 * Copy the nodeid from a producing subop's output into a dependent
 * subop's input.  Only entry-producing opcodes are recognised; depending
 * on a non-entry op is a caller bug and triggers a warning so the bad
 * dispatch is visible instead of silently sending nodeid 0.
 */
static void fuse_compound_propagate_nodeid(struct fuse_args *dep,
					   const struct fuse_args *src)
{
	const struct fuse_entry_out *entry_out;

	if (src->out_numargs == 0)
		return;

	switch (src->opcode) {
	case FUSE_LOOKUP:
	case FUSE_MKNOD:
	case FUSE_MKDIR:
	case FUSE_SYMLINK:
	case FUSE_LINK:
	case FUSE_CREATE:
	case FUSE_TMPFILE:
		entry_out = src->out_args[0].value;
		if (entry_out)
			dep->nodeid = entry_out->nodeid;
		break;
	default:
		WARN_ONCE(1, "fuse: compound dep on non-entry opcode %u\n",
			  src->opcode);
		break;
	}
}

/* Fallback: dispatch each subop individually as a normal FUSE request. */
static void fuse_compound_send_legacy(struct fuse_mount *fm,
				      struct fuse_compound_op *ops,
				      unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		struct fuse_compound_op *cur = &ops[i];

		if (cur->dep_index != FUSE_COMPOUND_NO_DEP) {
			struct fuse_compound_op *dep;

			/*
			 * dep_index must refer to an earlier subop in the
			 * same compound so its result is already available.
			 * A forward or self reference is a caller bug; fail
			 * the subop loudly instead of reading uninitialised
			 * memory.
			 */
			if (WARN_ON_ONCE(cur->dep_index >= i)) {
				*cur->error = -EINVAL;
				continue;
			}
			dep = &ops[cur->dep_index];

			if (*dep->error) {
				*cur->error = *dep->error;
				continue;
			}
			fuse_compound_propagate_nodeid(cur->arg, dep->arg);
		}
		*cur->error = fuse_simple_request(fm, cur->arg);
	}
}

/*
 * Send a compound request.  Per-subop status is reported via the @error
 * pointer of each fuse_compound_op; the return value is 0 if the
 * compound was dispatched (whether server-side or via the legacy
 * fallback) and a negative errno only if dispatch itself failed.
 *
 * Server-side decline signaling:
 *   -ENOSYS      Compound is not implemented at all.  Disable the
 *                feature for this connection and fall back to legacy
 *                dispatch for this and every subsequent request.
 *   -EOPNOTSUPP  This specific compound combination is not supported,
 *                but the feature remains usable.  Fall back to legacy
 *                dispatch for this request only; leave fc->compound_ops
 *                set so future requests may still go through compound.
 *
 * (ENOTSUPP is a Linux-internal errno > 511 and is rejected by
 * fuse_dev_do_write(), so a userspace server cannot signal it.)
 */
int fuse_compound_send(struct fuse_mount *fm,
		       struct fuse_compound_op *ops, unsigned int count)
{
	struct fuse_conn *fc = fm->fc;
	struct fuse_compound_args compound = {
		.args = { .opcode = FUSE_COMPOUND, },
		.ops = ops,
		.count = count,
	};
	int ret;

	if (WARN_ON_ONCE(count == 0))
		return -EINVAL;

	if (!fc->compound_ops) {
		fuse_compound_send_legacy(fm, ops, count);
		return 0;
	}

	ret = fuse_simple_request(fm, &compound.args);
	if (ret == -ENOSYS)
		fc->compound_ops = 0;
	if (ret == -ENOSYS || ret == -EOPNOTSUPP) {
		fuse_compound_send_legacy(fm, ops, count);
		return 0;
	}
	return ret;
}
