/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2025 Allegro DVT.
 * Author: Yassine OUAISSA <yassine.ouaissa@allegrodvt.fr>
 */

#ifndef __AL_CODEC_DBGFS_H__
#define __AL_CODEC_DBGFS_H__

struct al_codec_dev;
struct al_dec_ctx;

/*
 * enum al_vdec_dbgfs_log_index  - used to get different debug information
 */
enum al_vdec_dbgfs_log_index {
	AL_VDEC_DBGFS_PICINFO,
	AL_VDEC_DBGFS_FORMAT,
	AL_VDEC_DBGFS_MAX,
};

/**
 * struct al_codec_dbgfs_ctx  - debugfs information for each context
 * @node:       list node for each inst
 * @dec_ctx:	struct al_dec_ctx
 * @ctx_id:     index of the context that the same with ctx->id
 */
struct al_codec_dbgfs_ctx {
	struct list_head node;
	struct al_dec_ctx *dec_ctx;
	int ctx_id;
};

/**
 * struct al_codec_dbgfs  - dbgfs information
 * @dbgfs_head:  list head used to link each context
 * @vcodec_root: codec dbgfs entry
 * @lock:	 lock used to protect dbgfs_buf
 * @buf:	 dbgfs buf used to store dbgfs cmd
 * @size:	 dbgfs buffer size
 * @ctx_count:   the count of total context
 */
struct al_codec_dbgfs {
	struct list_head dbgfs_head;
	struct dentry *vcodec_root;
	struct mutex lock;
	char buf[1024];
	int size;
	int ctx_count;
};

#if defined(CONFIG_DEBUG_FS)
void al_codec_dbgfs_create(struct al_dec_ctx *ctx);
void al_codec_dbgfs_remove(struct al_codec_dev *codec, int ctx_id);
void al_codec_dbgfs_init(void *codec);
void al_codec_dbgfs_deinit(struct al_codec_dbgfs *dbgfs);
#else
static inline void al_codec_dbgfs_create(struct al_dec_ctx *ctx)
{
}
static inline void al_codec_dbgfs_remove(struct al_codec_dev *codec, int ctx_id)
{
}
static inline void al_codec_dbgfs_init(void *codec)
{
}
static inline void al_codec_dbgfs_deinit(struct al_codec_dbgfs *dbgfs)
{
}
#endif /* CONFIG_DEBUG_FS */
#endif /* __AL_CODEC_DBGFS_H__ */
