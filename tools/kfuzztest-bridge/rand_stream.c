// SPDX-License-Identifier: GPL-2.0
/*
 * Implements a cached file-reader for iterating over a byte stream of
 * pseudo-random data
 *
 * Copyright 2025 Google LLC
 */
#include "rand_stream.h"

static int refill(struct rand_stream *rs)
{
	rs->valid_bytes = fread(rs->buffer, sizeof(char), rs->buffer_size, rs->source);
	rs->buffer_pos = 0;
	if (rs->valid_bytes != rs->buffer_size && ferror(rs->source))
		return ferror(rs->source);
	return 0;
}

struct rand_stream *new_rand_stream(const char *path_to_file, size_t cache_size)
{
	struct rand_stream *rs;

	rs = malloc(sizeof(*rs));
	if (!rs)
		return NULL;

	rs->valid_bytes = 0;
	rs->source = fopen(path_to_file, "rb");
	if (!rs->source) {
		free(rs);
		return NULL;
	}

	if (fseek(rs->source, 0, SEEK_END)) {
		fclose(rs->source);
		free(rs);
		return NULL;
	}
	rs->source_size = ftell(rs->source);

	if (fseek(rs->source, 0, SEEK_SET)) {
		fclose(rs->source);
		free(rs);
		return NULL;
	}

	rs->buffer = malloc(cache_size);
	if (!rs->buffer) {
		fclose(rs->source);
		free(rs);
		return NULL;
	}
	rs->buffer_size = cache_size;
	return rs;
}

void destroy_rand_stream(struct rand_stream *rs)
{
	fclose(rs->source);
	free(rs->buffer);
	free(rs);
}

int next_byte(struct rand_stream *rs, char *ret)
{
	int res;

	if (rs->buffer_pos >= rs->valid_bytes) {
		res = refill(rs);
		if (res)
			return res;
		if (rs->valid_bytes == 0)
			return STREAM_EOF;
	}
	*ret = rs->buffer[rs->buffer_pos++];
	return 0;
}
