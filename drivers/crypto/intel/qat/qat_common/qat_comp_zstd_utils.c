// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 Intel Corporation */
#include <linux/module.h>
#include <linux/byteorder/generic.h>
#include <linux/zstd_lib.h>

#include "qat_comp_zstd_utils.h"

#define ML_BITS		4
#define ML_MASK		((1U << ML_BITS) - 1)
#define RUN_BITS	(8 - ML_BITS)
#define RUN_MASK	((1U << RUN_BITS) - 1)
#define LZ4MINMATCH	2

/*
 * Implement the same algorithm as the QAT ZSTD sequence producer plugin,
 * to decode LZ4s formatted data into ZSTD_Sequence format.
 */
size_t qat_alg_dec_lz4s(ZSTD_Sequence *out_seqs, size_t out_seqs_capacity,
			unsigned char *lz4s_buff, unsigned int lz4s_buff_size,
			unsigned char *literals, unsigned int *lit_len)
{
	unsigned char *end_ip = lz4s_buff + lz4s_buff_size;
	unsigned int hist_literal_len = 0;
	unsigned char *ip = lz4s_buff;
	size_t seqs_idx = 0;

	*lit_len = 0;

	if (!lz4s_buff_size)
		return 0;

	while (ip < end_ip) {
		size_t length = 0;
		size_t offset = 0;
		size_t literal_len = 0, match_len = 0;

		/* get literal length */
		unsigned const token = *ip++;

		length = token >> ML_BITS;
		if (length == RUN_MASK) {
			unsigned int s;

			do {
				s = *ip++;
				length += s;
			} while (s == 255);
		}

		literal_len = length;

		{
			u8 *start = ip;
			u8 *dest = literals;
			u8 *dest_end = literals + length;

			do {
				__builtin_memcpy(dest, start, QAT_ZSTD_LIT_COPY_LEN);
				dest += QAT_ZSTD_LIT_COPY_LEN;
				start += QAT_ZSTD_LIT_COPY_LEN;
			} while (dest < dest_end);
		}

		literals += length;
		*lit_len += length;

		ip += length;
		if (ip == end_ip) { /* Meet the end of the LZ4 sequence */
			literal_len += hist_literal_len;
			out_seqs[seqs_idx].litLength = literal_len;
			out_seqs[seqs_idx].offset = offset;
			out_seqs[seqs_idx].matchLength = match_len;
			break;
		}

		/* get matchPos */
		offset = le16_to_cpu(*(__le16 *)ip);
		ip += 2;

		/* get match length */
		length = token & ML_MASK;
		if (length == ML_MASK) {
			unsigned int s;

			do {
				s = *ip++;
				length += s;
			} while (s == 255);
		}
		if (length != 0) {
			length += LZ4MINMATCH;
			match_len = (unsigned short)length;
			literal_len += hist_literal_len;

			/* update ZSTD_Sequence */
			out_seqs[seqs_idx].offset = offset;
			out_seqs[seqs_idx].litLength = literal_len;
			out_seqs[seqs_idx].matchLength = match_len;
			hist_literal_len = 0;
			++seqs_idx;
			if (seqs_idx >= (out_seqs_capacity - 1)) {
				pr_debug("[%s]: qat zstd sequence overflow (seqs_idx:%lu, out_seqs_capacity:%lu, lz4s_buff_size:%u)\n",
					 __func__, seqs_idx, out_seqs_capacity, lz4s_buff_size);
				return -1;
			}
		} else {
			if (literal_len > 0) {
				/*
				 * When match length is 0, the literalLen needs to be
				 * temporarily stored and processed together with the next data
				 * block.
				 */
				hist_literal_len += literal_len;
			}
		}
	}

	return ++seqs_idx;
}
