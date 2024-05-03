#include <linux/kernel.h>
#include <linux/lz4.h>
#include <linux/vmalloc.h>

#include "backend_lz4.h"

static void *lz4_create(void)
{
	return vmalloc(LZ4_MEM_COMPRESS);
}

static void lz4_destroy(void *ctx)
{
	vfree(ctx);
}

static int lz4_compress(void *ctx, const unsigned char *src,
			unsigned char *dst, size_t *dst_len)
{
	int ret;

	ret = LZ4_compress_default(src, dst, PAGE_SIZE, *dst_len, ctx);
	if (!ret)
		return -EINVAL;
	*dst_len = ret;
	return 0;
}

static int lz4_decompress(void *ctx, const unsigned char *src,
			  size_t src_len, unsigned char *dst)
{
	int dst_len = PAGE_SIZE;
	int ret;

	ret = LZ4_decompress_safe(src, dst, src_len, dst_len);
	if (ret < 0)
		return -EINVAL;
	return 0;
}

struct zcomp_backend backend_lz4 = {
	.compress	= lz4_compress,
	.decompress	= lz4_decompress,
	.create_ctx	= lz4_create,
	.destroy_ctx	= lz4_destroy,
	.name		= "lz4",
};
