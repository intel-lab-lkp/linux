// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE: Filesystem in Userspace
 * Copyright (C) 2025
 *
 * Compound operations for FUSE - batch multiple operations into a single
 * request to reduce round trips between kernel and userspace.
 */

#include "fuse_i.h"

/*
 * Compound request builder, state tracker, and args pointer storage
 */
struct fuse_compound_req {
	struct fuse_mount *fm;
	struct fuse_compound_in compound_header;
	struct fuse_compound_out result_header;
	int op_errors[FUSE_MAX_COMPOUND_OPS];
	struct fuse_args *op_args[FUSE_MAX_COMPOUND_OPS];
};

struct fuse_compound_req *fuse_compound_alloc(struct fuse_mount *fm, u32 flags)
{
	struct fuse_compound_req *compound;

	compound = kzalloc(sizeof(*compound), GFP_KERNEL);
	if (!compound)
		return NULL;

	compound->fm = fm;
	compound->compound_header.flags = flags;

	return compound;
}

int fuse_compound_add(struct fuse_compound_req *compound, struct fuse_args *args)
{
	if (!compound ||
	    compound->compound_header.count >= FUSE_MAX_COMPOUND_OPS)
		return -EINVAL;

	if (args->in_pages)
		return -EINVAL;

	compound->op_args[compound->compound_header.count] = args;
	compound->compound_header.count++;
	return 0;
}

static void *fuse_copy_resp_data_per_req(const struct fuse_args *args,
					  char *resp)
{
	const struct fuse_arg *arg;
	int i;

	for (i = 0; i < args->out_numargs; i++) {
		arg = &args->out_args[i];
		memcpy(arg->value, resp, arg->size);
		resp += arg->size;
	}

	return resp;
}

int fuse_compound_get_error(struct fuse_compound_req *compound, int op_idx)
{
	return compound->op_errors[op_idx];
}

static void *fuse_compound_parse_one_op(struct fuse_compound_req *compound,
					 int op_index, char *response,
					 char *response_end)
{
	struct fuse_out_header *op_hdr = (struct fuse_out_header *)response;
	struct fuse_args *args = compound->op_args[op_index];

	if (op_hdr->len < sizeof(struct fuse_out_header))
		return NULL;

	if (response + op_hdr->len > response_end)
		return NULL;

	if (op_hdr->error)
		compound->op_errors[op_index] = op_hdr->error;
	else
		fuse_copy_resp_data_per_req(args, response +
					    sizeof(struct fuse_out_header));
	/* in case of error, we still need to advance to the next op */
	return response + op_hdr->len;
}

static int fuse_compound_parse_resp(struct fuse_compound_req *compound,
				     char *response, size_t response_size)
{
	char *response_end = response + response_size;
	int req_count;
	int i;

	req_count = min(compound->compound_header.count,
			compound->result_header.count);

	for (i = 0; i < req_count; i++) {
		response = fuse_compound_parse_one_op(compound, i, response,
						      response_end);
		if (!response)
			return -EIO;
	}

	return 0;
}

/*
 * Build a single operation request in the buffer
 *
 * Returns the new buffer position after writing the operation.
 */
static char *fuse_compound_build_one_op(struct fuse_conn *fc,
					 struct fuse_args *op_args,
					 char *buffer_pos)
{
	struct fuse_in_header *hdr;
	size_t needed_size = sizeof(struct fuse_in_header);
	int j;

	for (j = 0; j < op_args->in_numargs; j++)
		needed_size += op_args->in_args[j].size;

	hdr = (struct fuse_in_header *)buffer_pos;
	memset(hdr, 0, sizeof(*hdr));
	hdr->len = needed_size;
	hdr->opcode = op_args->opcode;
	hdr->nodeid = op_args->nodeid;
	hdr->uid = from_kuid(fc->user_ns, current_fsuid());
	hdr->gid = from_kgid(fc->user_ns, current_fsgid());
	hdr->pid = pid_nr_ns(task_pid(current), fc->pid_ns);
	buffer_pos += sizeof(*hdr);

	for (j = 0; j < op_args->in_numargs; j++) {
		memcpy(buffer_pos, op_args->in_args[j].value,
		       op_args->in_args[j].size);
		buffer_pos += op_args->in_args[j].size;
	}

	return buffer_pos;
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
	unsigned int req_count = compound->compound_header.count;
	size_t total_expected_out_size = 0;
	size_t actual_response_size;
	size_t buffer_size = 0;
	void *resp_payload_buffer;
	char *buffer_pos;
	void *buffer = NULL;
	ssize_t ret;
	int i, j;

	for (i = 0; i < req_count; i++) {
		struct fuse_args *op_args = compound->op_args[i];
		size_t needed_size = sizeof(struct fuse_in_header);

		for (j = 0; j < op_args->in_numargs; j++)
			needed_size += op_args->in_args[j].size;

		buffer_size += needed_size;

		for (j = 0; j < op_args->out_numargs; j++)
			total_expected_out_size += op_args->out_args[j].size;
	}

	buffer = kmalloc(buffer_size, GFP_KERNEL | __GFP_ZERO);
	if (!buffer)
		return -ENOMEM;

	buffer_pos = buffer;
	for (i = 0; i < req_count; i++)
		buffer_pos = fuse_compound_build_one_op(fc,
							compound->op_args[i],
							buffer_pos);

	compound->compound_header.result_size = total_expected_out_size;

	args.in_args[0].size = sizeof(compound->compound_header);
	args.in_args[0].value = &compound->compound_header;
	args.in_args[1].size = buffer_size;
	args.in_args[1].value = buffer;

	buffer_size = total_expected_out_size +
		      (req_count * sizeof(struct fuse_out_header));

	resp_payload_buffer = kmalloc(buffer_size, GFP_KERNEL | __GFP_ZERO);
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
		goto out;

	actual_response_size = args.out_args[1].size;

	ret = fuse_compound_parse_resp(compound, (char *)resp_payload_buffer,
				       actual_response_size);
out:
	kfree(resp_payload_buffer);
out_free_buffer:
	kfree(buffer);
	return ret;
}
