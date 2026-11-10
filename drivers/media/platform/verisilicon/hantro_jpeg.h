/* SPDX-License-Identifier: GPL-2.0+ */

struct hantro_ctx;
struct vb2_buffer;

#define JPEG_HEADER_SIZE	624
#define JPEG_QUANT_SIZE		64

struct hantro_jpeg_ctx {
	int width;
	int height;
	int quality;
	unsigned char *buffer;
	unsigned char hw_luma_qtable[JPEG_QUANT_SIZE];
	unsigned char hw_chroma_qtable[JPEG_QUANT_SIZE];
};

void hantro_jpeg_header_assemble(struct hantro_jpeg_ctx *ctx);
int hantro_jpeg_enc_check_job(struct hantro_ctx *ctx,
			      struct vb2_buffer *dst_buf);
