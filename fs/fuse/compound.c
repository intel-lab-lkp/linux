// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE: Filesystem in Userspace
 * Copyright (C) 2025
 *
 * This file implements compound operations for FUSE, allowing multiple
 * operations to be batched into a single request to reduce round trips
 * between kernel and userspace.
 */

#include "fuse_i.h"

/*
 * Compound request builder and state tracker
 *
 * This structure manages the lifecycle of a compound FUSE request, from building
 * the request by serializing multiple operations into a single buffer, through
 * sending it to userspace, to parsing the compound response back into individual
 * operation results.
 */
struct fuse_compound_req {
	struct fuse_mount *fm;
	struct fuse_compound_in compound_header;
	struct fuse_compound_out result_header;

	size_t total_size;			/* Total size of serialized operations */
	char *buffer;				/* Buffer holding serialized requests */
	size_t buffer_pos;			/* Current write position in buffer */
	size_t buffer_size;			/* Total allocated buffer size */

	size_t total_expected_out_size;		/* Sum of expected output sizes */

	/* Per-operation error codes */
	int op_errors[FUSE_MAX_COMPOUND_OPS];
	/* Original fuse_args for response parsing */
	struct fuse_args *op_args[FUSE_MAX_COMPOUND_OPS];

	bool parsed;				/* Prevent double-parsing of response */
};

struct fuse_compound_req *fuse_compound_alloc(struct fuse_mount *fm,
					       uint32_t flags)
{
	struct fuse_compound_req *compound;

	compound = kzalloc(sizeof(*compound), GFP_KERNEL);
	if (!compound)
		return ERR_PTR(-ENOMEM);

	compound->fm = fm;
	compound->compound_header.flags = flags;
	compound->buffer_size = PAGE_SIZE;
	compound->buffer = kvmalloc(compound->buffer_size, GFP_KERNEL);
	if (!compound->buffer) {
		kfree(compound);
		return ERR_PTR(-ENOMEM);
	}
	return compound;
}

void fuse_compound_free(struct fuse_compound_req *compound)
{
	if (!compound)
		return;

	kvfree(compound->buffer);
	kfree(compound);
}

static int fuse_compound_validate_header(struct fuse_compound_req *compound)
{
	struct fuse_compound_in *in_header = &compound->compound_header;
	size_t offset = 0;
	int i;

	if (compound->buffer_pos > compound->buffer_size)
		return -EINVAL;

	if (!compound || !compound->buffer)
		return -EINVAL;

	if (compound->buffer_pos < sizeof(struct fuse_in_header))
		return -EINVAL;

	if (in_header->count == 0 || in_header->count > FUSE_MAX_COMPOUND_OPS)
		return -EINVAL;

	for (i = 0; i < in_header->count; i++) {
		const struct fuse_in_header *op_hdr;

		if (offset + sizeof(struct fuse_in_header) >
		    compound->buffer_pos) {
			pr_info_ratelimited("FUSE: compound operation %d header extends beyond buffer (offset %zu + header size %zu > buffer pos %zu)\n",
					    i, offset,
					    sizeof(struct fuse_in_header),
					    compound->buffer_pos);
			return -EINVAL;
		}

		op_hdr = (const struct fuse_in_header *)(compound->buffer +
							 offset);

		if (op_hdr->len < sizeof(struct fuse_in_header)) {
			pr_info_ratelimited("FUSE: compound operation %d has invalid length %u (minimum %zu bytes)\n",
					    i, op_hdr->len,
					    sizeof(struct fuse_in_header));
			return -EINVAL;
		}

		if (offset + op_hdr->len > compound->buffer_pos) {
			pr_info_ratelimited("FUSE: compound operation %d extends beyond buffer (offset %zu + length %u > buffer pos %zu)\n",
					    i, offset, op_hdr->len,
					    compound->buffer_pos);
			return -EINVAL;
		}

		if (op_hdr->opcode == 0 || op_hdr->opcode == FUSE_COMPOUND) {
			pr_info_ratelimited("FUSE: compound operation %d has invalid opcode %u (cannot be 0 or FUSE_COMPOUND)\n",
					    i, op_hdr->opcode);
			return -EINVAL;
		}

		if (op_hdr->nodeid == 0) {
			pr_info_ratelimited("FUSE: compound operation %d has invalid node ID 0\n",
					    i);
			return -EINVAL;
		}

		offset += op_hdr->len;
	}

	if (offset != compound->buffer_pos) {
		pr_info_ratelimited("FUSE: compound buffer size mismatch (calculated %zu bytes, actual %zu bytes)\n",
				    offset, compound->buffer_pos);
		return -EINVAL;
	}

	return 0;
}

int fuse_compound_add(struct fuse_compound_req *compound,
		      struct fuse_args *args)
{
	struct fuse_in_header *hdr;
	size_t args_size = 0;
	size_t needed_size;
	size_t expected_out_size = 0;
	int i;

	if (!compound ||
	    compound->compound_header.count >= FUSE_MAX_COMPOUND_OPS)
		return -EINVAL;

	if (args->in_pages)
		return -EINVAL;

	for (i = 0; i < args->in_numargs; i++)
		args_size += args->in_args[i].size;

	for (i = 0; i < args->out_numargs; i++)
		expected_out_size += args->out_args[i].size;

	needed_size = sizeof(struct fuse_in_header) + args_size;

	if (compound->buffer_pos + needed_size > compound->buffer_size) {
		size_t new_size = max(compound->buffer_size * 2,
				      compound->buffer_pos + needed_size);
		char *new_buffer;

		new_size = round_up(new_size, PAGE_SIZE);
		new_buffer = kvrealloc(compound->buffer, new_size,
				       GFP_KERNEL);
		if (!new_buffer)
			return -ENOMEM;
		compound->buffer = new_buffer;
		compound->buffer_size = new_size;
	}

	/* Build request header */
	hdr = (struct fuse_in_header *)(compound->buffer +
					compound->buffer_pos);
	memset(hdr, 0, sizeof(*hdr));
	hdr->len = needed_size;
	hdr->opcode = args->opcode;
	hdr->nodeid = args->nodeid;
	hdr->uid = from_kuid(compound->fm->fc->user_ns, current_fsuid());
	hdr->gid = from_kgid(compound->fm->fc->user_ns, current_fsgid());
	hdr->pid = pid_nr_ns(task_pid(current), compound->fm->fc->pid_ns);
	hdr->unique = fuse_get_unique(&compound->fm->fc->iq);
	compound->buffer_pos += sizeof(*hdr);

	for (i = 0; i < args->in_numargs; i++) {
		memcpy(compound->buffer + compound->buffer_pos,
		       args->in_args[i].value, args->in_args[i].size);
		compound->buffer_pos += args->in_args[i].size;
	}

	compound->total_expected_out_size += expected_out_size;

	/* Store args for response parsing */
	compound->op_args[compound->compound_header.count] = args;

	compound->compound_header.count++;
	compound->total_size += needed_size;

	return 0;
}

static void *fuse_copy_response_data(struct fuse_args *args,
				     char *response_data)
{
	size_t copied = 0;
	int arg_idx;

	for (arg_idx = 0; arg_idx < args->out_numargs; arg_idx++) {
		struct fuse_arg current_arg = args->out_args[arg_idx];
		size_t arg_size;

		/* Last argument with out_pages: copy to pages */
		if (arg_idx == args->out_numargs - 1 && args->out_pages) {
			/*
			 * External payload (in the last out arg)
			 * is not supported at the moment
			 */
			return response_data;
		}

		arg_size = current_arg.size;

		if (current_arg.value && arg_size > 0) {
			memcpy(current_arg.value,
			       (char *)response_data + copied,
			       arg_size);
			copied += arg_size;
		}
	}

	return (char *)response_data + copied;
}

int fuse_compound_get_error(struct fuse_compound_req *compound, int op_idx)
{
	return compound->op_errors[op_idx];
}

static void *fuse_compound_parse_one_op(struct fuse_compound_req *compound,
					int op_index, void *op_out_data,
					void *response_end)
{
	struct fuse_out_header *op_hdr = op_out_data;
	struct fuse_args *args = compound->op_args[op_index];

	if (op_hdr->len < sizeof(struct fuse_out_header))
		return NULL;

	/* Check if the entire operation response fits in the buffer */
	if ((char *)op_out_data + op_hdr->len > (char *)response_end)
		return NULL;

	if (op_hdr->error != 0)
		compound->op_errors[op_index] = op_hdr->error;

	if (args && op_hdr->len > sizeof(struct fuse_out_header))
		return fuse_copy_response_data(args, op_out_data +
					       sizeof(struct fuse_out_header));

	/* No response data, just advance past the header */
	return (char *)op_out_data + op_hdr->len;
}

static int fuse_compound_parse_resp(struct fuse_compound_req *compound,
				    uint32_t count, void *response,
				    size_t response_size)
{
	void *op_out_data = response;
	void *response_end = (char *)response + response_size;
	int i;

	if (compound->parsed)
		return 0;

	if (!response || response_size < sizeof(struct fuse_out_header))
		return -EIO;

	for (i = 0; i < count && i < compound->result_header.count; i++) {
		op_out_data = fuse_compound_parse_one_op(compound, i,
							 op_out_data,
							 response_end);
		if (!op_out_data)
			return -EIO;
	}

	compound->parsed = true;
	return 0;
}

ssize_t fuse_compound_send(struct fuse_compound_req *compound)
{
	struct fuse_args args = {
		.opcode = FUSE_COMPOUND,
		.nodeid = 0,
		.in_numargs = 2,
		.out_numargs = 2,
		.out_argvar = true,
	};
	size_t expected_response_size;
	size_t total_buffer_size;
	size_t actual_response_size;
	void *resp_payload;
	ssize_t ret;

	if (!compound) {
		pr_info_ratelimited("FUSE: compound request is NULL in %s\n",
				    __func__);
		return -EINVAL;
	}

	if (compound->compound_header.count == 0) {
		pr_info_ratelimited("FUSE: compound request contains no operations\n");
		return -EINVAL;
	}

	expected_response_size = compound->total_expected_out_size;
	total_buffer_size = expected_response_size +
		(compound->compound_header.count *
		 sizeof(struct fuse_out_header));

	resp_payload = kvmalloc(total_buffer_size, GFP_KERNEL | __GFP_ZERO);
	if (!resp_payload)
		return -ENOMEM;

	compound->compound_header.result_size = expected_response_size;

	args.in_args[0].size = sizeof(compound->compound_header);
	args.in_args[0].value = &compound->compound_header;
	args.in_args[1].size = compound->buffer_pos;
	args.in_args[1].value = compound->buffer;

	args.out_args[0].size = sizeof(compound->result_header);
	args.out_args[0].value = &compound->result_header;
	args.out_args[1].size = total_buffer_size;
	args.out_args[1].value = resp_payload;

	ret = fuse_compound_validate_header(compound);
	if (ret)
		goto out;

	ret = fuse_compound_request(compound->fm, &args);
	if (ret)
		goto out;

	actual_response_size = args.out_args[1].size;

	if (actual_response_size < sizeof(struct fuse_compound_out)) {
		pr_info_ratelimited("FUSE: compound response too small (%zu bytes, minimum %zu bytes)\n",
				    actual_response_size,
				    sizeof(struct fuse_compound_out));
		ret = -EINVAL;
		goto out;
	}

	ret = fuse_compound_parse_resp(compound, compound->result_header.count,
				       (char *)resp_payload,
				       actual_response_size);
out:
	kvfree(resp_payload);
	return ret;
}
