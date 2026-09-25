/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
/*
 * Copyright (c) 2026 Samsung Electronics Co., Ltd.
 * Author: Michal Wilczynski <m.wilczynski@samsung.com>
 *
 * lz4_kernel_api.h -- the LZ4 API this directory exports
 *
 * lz4_deps.h renames upstream's entry points to __lz4_* so that the kernel's
 * own definitions can use the real names; this header undoes that renaming
 * and declares what lib/lz4 exports.
 *
 * The same functions are declared to callers in <linux/lz4.h>, in terms of
 * opaque stream types.  We cannot include that header here -- it and
 * upstream's lz4.h describe the same library and collide -- so the
 * declarations are repeated, expressed in upstream's own types.  Keep the two
 * in step; <linux/lz4.h> is the one callers compile against.
 *
 * Include after upstream/lz4.c or upstream/lz4hc.c.
 */

#undef LZ4_compress_fast
#undef LZ4_compress_default
#undef LZ4_compress_destSize
#undef LZ4_resetStream
#undef LZ4_loadDict
#undef LZ4_saveDict
#undef LZ4_compress_fast_continue

#undef LZ4_decompress_safe
#undef LZ4_decompress_safe_partial
#undef LZ4_decompress_fast
#undef LZ4_setStreamDecode
#undef LZ4_decompress_safe_continue
#undef LZ4_decompress_fast_continue
#undef LZ4_decompress_safe_usingDict
#undef LZ4_decompress_fast_usingDict

#undef LZ4_compress_HC
#undef LZ4_resetStreamHC
#undef LZ4_loadDictHC
#undef LZ4_compress_HC_continue
#undef LZ4_saveDictHC

/*
 * The objects that do not build lz4hc.c never see this type, and it is only
 * ever used through a pointer here.  Upstream declares it the same way.
 */
typedef union LZ4_streamHC_u LZ4_streamHC_t;

/* Compression.  wrkmem is LZ4_MEM_COMPRESS bytes, supplied by the caller. */
int LZ4_compress_default(const char *source, char *dest, int inputSize,
			 int maxOutputSize, void *wrkmem);
int LZ4_compress_fast(const char *source, char *dest, int inputSize,
		      int maxOutputSize, int acceleration, void *wrkmem);
int LZ4_compress_destSize(const char *source, char *dest, int *sourceSizePtr,
			  int targetDestSize, void *wrkmem);

/* Streaming compression. */
void LZ4_resetStream(LZ4_stream_t *LZ4_stream);
int LZ4_loadDict(LZ4_stream_t *streamPtr, const char *dictionary,
		 int dictSize);
int LZ4_saveDict(LZ4_stream_t *streamPtr, char *safeBuffer, int dictSize);
int LZ4_compress_fast_continue(LZ4_stream_t *streamPtr, const char *src,
			       char *dst, int srcSize, int maxDstSize,
			       int acceleration);

/* Decompression. */
int LZ4_decompress_safe(const char *source, char *dest, int compressedSize,
			int maxDecompressedSize);
int LZ4_decompress_safe_partial(const char *source, char *dest,
				int compressedSize, int targetOutputSize,
				int maxDecompressedSize);
int LZ4_decompress_fast(const char *source, char *dest, int originalSize);
int LZ4_setStreamDecode(LZ4_streamDecode_t *LZ4_streamDecode,
			const char *dictionary, int dictSize);
int LZ4_decompress_safe_continue(LZ4_streamDecode_t *LZ4_streamDecode,
				 const char *source, char *dest,
				 int compressedSize, int maxDecompressedSize);
int LZ4_decompress_fast_continue(LZ4_streamDecode_t *LZ4_streamDecode,
				 const char *source, char *dest,
				 int originalSize);
int LZ4_decompress_safe_usingDict(const char *source, char *dest,
				  int compressedSize, int maxDecompressedSize,
				  const char *dictStart, int dictSize);
int LZ4_decompress_fast_usingDict(const char *source, char *dest,
				  int originalSize, const char *dictStart,
				  int dictSize);

/* HC compression.  wrkmem is LZ4HC_MEM_COMPRESS bytes. */
int LZ4_compress_HC(const char *src, char *dst, int srcSize, int dstCapacity,
		    int compressionLevel, void *wrkmem);
void LZ4_resetStreamHC(LZ4_streamHC_t *streamHCPtr, int compressionLevel);
int LZ4_loadDictHC(LZ4_streamHC_t *streamHCPtr, const char *dictionary,
		   int dictSize);
int LZ4_compress_HC_continue(LZ4_streamHC_t *streamHCPtr, const char *src,
			     char *dst, int srcSize, int maxDstSize);
int LZ4_saveDictHC(LZ4_streamHC_t *streamHCPtr, char *safeBuffer,
		   int maxDictSize);
