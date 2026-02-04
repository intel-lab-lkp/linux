/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _ZCOMP_H_
#define _ZCOMP_H_

#include <linux/mutex.h>

#define ZCOMP_PARAM_NOT_SET	INT_MIN

struct deflate_params {
	s32 winbits;
};

/*
 * Immutable driver (backend) parameters. The driver may attach private
 * data to it (e.g. driver representation of the dictionary, etc.).
 *
 * This data is kept per-comp and is shared among execution contexts.
 */
struct zcomp_params {
	void *dict;
	size_t dict_sz;
	s32 level;
	union {
		struct deflate_params deflate;
	};
	bool zstrm_mgmt;

	void *drv_data;
};

/*
 * Run-time driver context - scratch buffers, etc. It is modified during
 * request execution (compression/decompression), cannot be shared, so
 * it's in per-CPU area or management by backend.
 */
struct zcomp_ctx {
	void *context;
};

struct zcomp_strm {
	bool zcomp_managed;
	/* lock used only for per-cpu streams */
	struct mutex lock;
	/* pointer to zcomp valid only for zcomp-managed streams */
	struct zcomp *comp;
	/* compression buffer */
	void *buffer;
	/* local copy of handle memory */
	void *local_copy;
	struct zcomp_ctx ctx;
};

struct zcomp_req {
	const unsigned char *src;
	const size_t src_len;

	unsigned char *dst;
	size_t dst_len;
};

enum zstrm_pref {
	ZSTRM_DEFAULT, /* always use the generic per-CPU stream */
	ZSTRM_PREFER_MGMT, /* try managed stream; fallback to per-CPU */
};

struct zcomp_ops {
	int (*compress)(struct zcomp_params *params, struct zcomp_ctx *ctx,
			struct zcomp_req *req);
	int (*decompress)(struct zcomp_params *params, struct zcomp_ctx *ctx,
			  struct zcomp_req *req);

	int (*create_ctx)(struct zcomp_params *params, struct zcomp_ctx *ctx);
	void (*destroy_ctx)(struct zcomp_ctx *ctx);

	int (*setup_params)(struct zcomp_params *params);
	void (*release_params)(struct zcomp_params *params);

	/*
	 * get_stream() needs to prepare zstrm->ctx, and backend must ensure
	 * returned stream sets zcomp_managed and match the per-cpu stream
	 * sizing: local_copy >= PAGE_SIZE, buffer >= 2 * PAGE_SIZE.
	 */
	struct zcomp_strm *(*get_stream)(struct zcomp_params *params);
	void (*put_stream)(struct zcomp_params *params,
			   struct zcomp_strm *zstrm);

	const char *name;
};

/* dynamic per-device compression frontend */
struct zcomp {
	struct zcomp_strm __percpu *stream;
	const struct zcomp_ops *ops;
	struct zcomp_params *params;
	struct hlist_node node;
};

int zcomp_cpu_up_prepare(unsigned int cpu, struct hlist_node *node);
int zcomp_cpu_dead(unsigned int cpu, struct hlist_node *node);
ssize_t zcomp_available_show(const char *comp, char *buf, ssize_t at);
bool zcomp_available_algorithm(const char *comp);

struct zcomp *zcomp_create(const char *alg, struct zcomp_params *params);
void zcomp_destroy(struct zcomp *comp);

struct zcomp_strm *zcomp_stream_get(struct zcomp *comp, enum zstrm_pref pref);
void zcomp_stream_put(struct zcomp_strm *zstrm);

int zcomp_compress(struct zcomp *comp, struct zcomp_strm *zstrm,
		   const void *src, unsigned int *dst_len);
int zcomp_decompress(struct zcomp *comp, struct zcomp_strm *zstrm,
		     const void *src, unsigned int src_len, void *dst);

#endif /* _ZCOMP_H_ */
