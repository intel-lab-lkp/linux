// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE: Filesystem in Userspace
 * Copyright (C) 2025-2026
 *
 * Compound operations for FUSE - batch multiple operations into a single
 * request to reduce round trips between kernel and userspace.
 */

#include "fuse_i.h"

struct fuse_compound_req *fuse_compound_alloc(struct fuse_mount *fm,
					       u32 max_count, u32 flags)
{
	struct fuse_compound_req *compound;

	if (max_count == 0)
		return NULL;

	compound = kzalloc(sizeof(*compound), GFP_KERNEL);
	if (!compound)
		return NULL;

	compound->max_count = max_count;
	compound->count = 0;
	compound->fm = fm;
	compound->compound_header.flags = flags;

	compound->op_errors = kcalloc(max_count, sizeof(int), GFP_KERNEL);
	if (!compound->op_errors)
		goto out_free_compound;

	compound->op_args = kcalloc(max_count, sizeof(struct fuse_args *),
				    GFP_KERNEL);
	if (!compound->op_args)
		goto out_free_op_errors;

	compound->op_converters = kcalloc(max_count,
					  sizeof(int (*)(struct fuse_compound_req *, unsigned int)),
					  GFP_KERNEL);
	if (!compound->op_converters)
		goto out_free_op_args;

	return compound;

out_free_op_args:
	kfree(compound->op_args);
out_free_op_errors:
	kfree(compound->op_errors);
out_free_compound:
	kfree(compound);
	return NULL;
}

void fuse_compound_free(struct fuse_compound_req *compound)
{
	kfree(compound->op_errors);
	kfree(compound->op_args);
	kfree(compound->op_converters);
	kfree(compound);
}

int fuse_compound_add(struct fuse_compound_req *compound,
			struct fuse_args *args,
			int (*converter)(struct fuse_compound_req *compound,
			unsigned int index))
{
	if (!compound || compound->count >= compound->max_count)
		return -EINVAL;

	if (args->in_pages)
		return -EINVAL;

	compound->op_args[compound->count] = args;
	compound->op_converters[compound->count] = converter;
	compound->count++;
	return 0;
}

static void fuse_copy_resp_data_per_req(const struct fuse_args *args,
				char *resp)
{
	const struct fuse_arg *arg;
	int i;

	for (i = 0; i < args->out_numargs; i++) {
		arg = &args->out_args[i];
		memcpy(arg->value, resp, arg->size);
		resp += arg->size;
	}
}

static char *fuse_compound_parse_one_op(struct fuse_compound_req *compound,
					char *response,
					char *response_end,
					int op_count)
{
	struct fuse_out_header *op_hdr = (struct fuse_out_header *)response;
	struct fuse_args *args;

	if (op_hdr->len < sizeof(struct fuse_out_header))
		return NULL;

	if (response + op_hdr->len > response_end)
		return NULL;

	if (op_count >= compound->max_count)
		return NULL;

	if (op_hdr->error) {
		compound->op_errors[op_count] = op_hdr->error;
	} else {
		args = compound->op_args[op_count];
		fuse_copy_resp_data_per_req(args, response +
					    sizeof(struct fuse_out_header));
	}

	/* In case of error, we still need to advance to the next op */
	return response + op_hdr->len;
}

static int fuse_compound_parse_resp(struct fuse_compound_req *compound,
				    char *response, char *response_end)
{
	int op_count = 0;

	while (response < response_end) {
		response = fuse_compound_parse_one_op(compound, response,
						      response_end, op_count);
		if (!response)
			return -EIO;
		op_count++;
	}

	return 0;
}

static int fuse_handle_compound_results(struct fuse_compound_req *compound,
					struct fuse_args *args)
{
	size_t actual_response_size;
	size_t buffer_size;
	char *resp_payload_buffer;
	int ret;

	buffer_size = compound->compound_header.result_size +
		      compound->count * sizeof(struct fuse_out_header);

	resp_payload_buffer = args->out_args[1].value;
	actual_response_size = args->out_args[1].size;

	if (actual_response_size <= buffer_size) {
		ret = fuse_compound_parse_resp(compound,
					       (char *)resp_payload_buffer,
					       resp_payload_buffer +
					       actual_response_size);
	} else {
		/* FUSE server sent more data than expected */
		ret = -EIO;
	}

	return ret;
}

/*
 * Build a single operation request in the buffer
 *
 * Returns the new buffer position after writing the operation.
 */
static char *fuse_compound_build_one_op(struct fuse_conn *fc,
					struct fuse_args *op_args,
					char *buffer_pos,
					unsigned int index)
{
	struct fuse_in_header *hdr;
	size_t needed_size = sizeof(struct fuse_in_header);
	int j;

	for (j = 0; j < op_args->in_numargs; j++)
		needed_size += op_args->in_args[j].size;

	hdr = (struct fuse_in_header *)buffer_pos;
	hdr->unique = index;
	hdr->len = needed_size;
	hdr->opcode = op_args->opcode;
	hdr->nodeid = op_args->nodeid;
	buffer_pos += sizeof(*hdr);

	for (j = 0; j < op_args->in_numargs; j++) {
		memcpy(buffer_pos, op_args->in_args[j].value,
		       op_args->in_args[j].size);
		buffer_pos += op_args->in_args[j].size;
	}

	return buffer_pos;
}

static ssize_t fuse_compound_fallback_separate(struct fuse_compound_req *compound)
{
	unsigned int req_count = compound->count;
	ssize_t ret = 0;
	unsigned int i;

	/* Try separate requests */
	for (i = 0; i < req_count; i++) {
		/* fill the current args from the already received responses */
		if (compound->op_converters[i])
			ret = compound->op_converters[i](compound, i);

		ret = fuse_simple_request(compound->fm, compound->op_args[i]);
		if (ret < 0) {
			compound->op_errors[i] = ret;
			if (!(compound->compound_header.flags & FUSE_COMPOUND_CONTINUE))
				break;
		}
	}

	return ret;
}

ssize_t fuse_compound_send(struct fuse_compound_req *compound)
{
	struct fuse_conn *fc = compound->fm->fc;
	struct fuse_args args = {
		.opcode = FUSE_COMPOUND,
		.in_numargs = 2,
		.out_numargs = 2,
		.out_argvar = true,
	};
	unsigned int req_count = compound->count;
	size_t total_expected_out_size = 0;
	size_t buffer_size = 0;
	void *resp_payload_buffer;
	char *buffer_pos;
	void *buffer = NULL;
	ssize_t ret;
	unsigned int i, j;

	for (i = 0; i < req_count; i++) {
		struct fuse_args *op_args = compound->op_args[i];
		size_t needed_size = sizeof(struct fuse_in_header);

		for (j = 0; j < op_args->in_numargs; j++)
			needed_size += op_args->in_args[j].size;

		buffer_size += needed_size;

		for (j = 0; j < op_args->out_numargs; j++)
			total_expected_out_size += op_args->out_args[j].size;
	}

	buffer = kzalloc(buffer_size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	buffer_pos = buffer;
	for (i = 0; i < req_count; i++) {
		if (compound->op_converters[i]) {
			ret = compound->op_converters[i](compound, i);
			if (ret < 0)
				goto out_free_buffer;
		}

		buffer_pos = fuse_compound_build_one_op(fc,
							compound->op_args[i],
							buffer_pos, i);
	}

	compound->compound_header.result_size = total_expected_out_size;

	args.in_args[0].size = sizeof(compound->compound_header);
	args.in_args[0].value = &compound->compound_header;
	args.in_args[1].size = buffer_size;
	args.in_args[1].value = buffer;

	buffer_size = total_expected_out_size +
		      req_count * sizeof(struct fuse_out_header);

	resp_payload_buffer = kzalloc(buffer_size, GFP_KERNEL);
	if (!resp_payload_buffer) {
		ret = -ENOMEM;
		goto out_free_buffer;
	}

	args.out_args[0].size = sizeof(compound->result_header);
	args.out_args[0].value = &compound->result_header;
	args.out_args[1].size = buffer_size;
	args.out_args[1].value = resp_payload_buffer;

	ret = fuse_simple_request(compound->fm, &args);
	if (ret < 0)
		goto fallback_separate;

	ret = fuse_handle_compound_results(compound, &args);
	if (ret == 0)
		goto out;

fallback_separate:
	/* Kernel tries to fallback to separate requests */
	if (!(compound->compound_header.flags & FUSE_COMPOUND_ATOMIC))
		ret = fuse_compound_fallback_separate(compound);

out:
	kfree(resp_payload_buffer);
out_free_buffer:
	kfree(buffer);
	return ret;
}
