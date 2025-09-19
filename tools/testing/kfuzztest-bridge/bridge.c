// SPDX-License-Identifier: GPL-2.0
/*
 * KFuzzTest tool for sending inputs into a KFuzzTest harness
 *
 * Copyright 2025 Google LLC
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "byte_buffer.h"
#include "encoder.h"
#include "input_lexer.h"
#include "input_parser.h"
#include "rand_stream.h"

static int invoke_kfuzztest_target(const char *target_name, const char *data, ssize_t data_size)
{
	ssize_t bytes_written;
	char *buf = NULL;
	int ret;
	int fd;

	if (asprintf(&buf, "/sys/kernel/debug/kfuzztest/%s/input", target_name) < 0)
		return -ENOMEM;

	fd = openat(AT_FDCWD, buf, O_WRONLY, 0);
	if (fd < 0) {
		ret = -errno;
		goto out_free;
	}

	/*
	 * A KFuzzTest target's debugfs handler expects the entire input to be
	 * written in a single contiguous blob. Treat partial writes as errors.
	 */
	bytes_written = write(fd, data, data_size);
	if (bytes_written != data_size) {
		ret = (bytes_written < 0) ? -errno : -EIO;
		goto out_close;
	}
	ret = 0;

out_close:
	if (close(fd) != 0 && ret == 0)
		ret = -errno;
out_free:
	free(buf);
	return ret;
}

static int invoke_one(const char *input_fmt, const char *fuzz_target, const char *input_filepath)
{
	struct ast_node *ast_prog;
	struct byte_buffer *bb;
	struct rand_stream *rs;
	struct token **tokens;
	size_t num_tokens;
	size_t num_bytes;
	int err;

	err = tokenize(input_fmt, &tokens, &num_tokens);
	if (err) {
		fprintf(stderr, "tokenization failed: %s\n", strerror(-err));
		return err;
	}

	err = parse(tokens, num_tokens, &ast_prog);
	if (err) {
		fprintf(stderr, "parsing failed: %s\n", strerror(-err));
		goto cleanup_tokens;
	}

	rs = new_rand_stream(input_filepath, 1024);
	if (!rs) {
		err = -ENOMEM;
		goto cleanup_ast;
	}

	err = encode(ast_prog, rs, &num_bytes, &bb);
	if (err == STREAM_EOF) {
		fprintf(stderr, "encoding failed: reached EOF in %s\n", input_filepath);
		err = -EINVAL;
		goto cleanup_rs;
	} else if (err) {
		fprintf(stderr, "encoding failed: %s\n", strerror(-err));
		goto cleanup_rs;
	}

	err = invoke_kfuzztest_target(fuzz_target, bb->buffer, (ssize_t)num_bytes);
	if (err)
		fprintf(stderr, "invocation failed: %s\n", strerror(-err));

	destroy_byte_buffer(bb);
cleanup_rs:
	destroy_rand_stream(rs);
cleanup_ast:
	destroy_ast_node(ast_prog);
cleanup_tokens:
	destroy_tokens(tokens, num_tokens);
	return err;
}

int main(int argc, char *argv[])
{
	if (argc != 4) {
		printf("Usage: %s <input-description> <fuzz-target-name> <input-file>\n", argv[0]);
		printf("For more detailed information see Documentation/dev-tools/kfuzztest.rst\n");
		return 1;
	}

	return invoke_one(argv[1], argv[2], argv[3]);
}
