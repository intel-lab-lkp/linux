// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 Allegro DVT.
 * Author: Yassine OUAISSA <yassine.ouaissa@allegrodvt.fr>
 */

#include <linux/debugfs.h>

#include "al_codec_dbgfs.h"
#include "al_vdec_drv.h"
#include "al_codec_util.h"

static void al_vdec_dbgfs_get_format_type(struct al_dec_ctx *ctx, char *buf,
					  int *used, int total)
{
	struct al_frame *frame = &ctx->src;
	int curr_len;

	switch (frame->fmt->pixelformat) {
	case V4L2_PIX_FMT_H264:
		curr_len = snprintf(buf + *used, total - *used,
				    "\toutput format: h264\n");
		break;
	case V4L2_PIX_FMT_HEVC:
		curr_len = snprintf(buf + *used, total - *used,
				    "\toutput format: hevc\n");
		break;
	case V4L2_PIX_FMT_JPEG:
		curr_len = snprintf(buf + *used, total - *used,
				    "\toutput format: jpeg\n");
		break;
	default:
		curr_len = snprintf(buf + *used, total - *used,
				    "\tunsupported output format: 0x%x\n",
				    frame->fmt->pixelformat);
	}
	*used += curr_len;
	frame = &ctx->dst;

	switch (frame->fmt->pixelformat) {
	case V4L2_PIX_FMT_NV12:
		curr_len = snprintf(buf + *used, total - *used,
				    "\tcapture format: NV12\n");
		break;
	case V4L2_PIX_FMT_NV16:
		curr_len = snprintf(buf + *used, total - *used,
				    "\tcapture format: NV16\n");
		break;
	case V4L2_PIX_FMT_P010:
		curr_len = snprintf(buf + *used, total - *used,
				    "\tcapture format: P010\n");
		break;
	case V4L2_PIX_FMT_YUV420:
		curr_len = snprintf(buf + *used, total - *used,
				    "\tcapture format: YUV420\n");
		break;
	case V4L2_PIX_FMT_YVU420:
		curr_len = snprintf(buf + *used, total - *used,
				    "\tcapture format: YVU420\n");
		break;
	default:
		curr_len = snprintf(buf + *used, total - *used,
				    "\tunsupported capture format: 0x%x\n",
				    frame->fmt->pixelformat);
	}
	*used += curr_len;
}

static void al_vdec_dbgfs_get_help(char *buf, int *used, int total)
{
	int curr_len;

	curr_len = snprintf(buf + *used, total - *used,
			    "help: (1: echo -'info' > dec 2: cat dec)\n");
	*used += curr_len;

	curr_len = snprintf(buf + *used, total - *used,
			    "\t-picinfo: get resolution\n");
	*used += curr_len;

	curr_len = snprintf(buf + *used, total - *used,
			    "\t-format: get output & capture queue format\n");
	*used += curr_len;
}

static ssize_t al_vdec_dbgfs_write(struct file *filp, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct al_codec_dev *codec = filp->private_data;
	struct al_codec_dbgfs *dbgfs = &codec->dbgfs;

	mutex_lock(&dbgfs->lock);
	dbgfs->size = simple_write_to_buffer(dbgfs->buf, sizeof(dbgfs->buf),
					     ppos, ubuf, count);
	mutex_unlock(&dbgfs->lock);
	if (dbgfs->size > 0)
		return count;

	return dbgfs->size;
}

static ssize_t al_vdec_dbgfs_read(struct file *filp, char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct al_codec_dev *codec = filp->private_data;
	struct al_codec_dbgfs *dbgfs = &codec->dbgfs;
	struct al_codec_dbgfs_ctx *dbgfs_ctx;
	struct al_dec_ctx *ctx;
	struct al_frame *frame;
	int total_len = 200 * (dbgfs->ctx_count == 0 ? 1 : dbgfs->ctx_count);
	int used_len = 0, curr_len, ret;
	bool dbgfs_index[AL_VDEC_DBGFS_MAX] = { 0 };
	char *buf = kmalloc(total_len, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;

	if (strstr(dbgfs->buf, "-help") || dbgfs->size == 1) {
		al_vdec_dbgfs_get_help(buf, &used_len, total_len);
		goto read_buffer;
	}

	if (strstr(dbgfs->buf, "-picinfo"))
		dbgfs_index[AL_VDEC_DBGFS_PICINFO] = true;

	if (strstr(dbgfs->buf, "-format"))
		dbgfs_index[AL_VDEC_DBGFS_FORMAT] = true;

	mutex_lock(&dbgfs->lock);
	list_for_each_entry(dbgfs_ctx, &dbgfs->dbgfs_head, node) {
		ctx = dbgfs_ctx->dec_ctx;
		frame = &ctx->src;

		curr_len = snprintf(buf + used_len, total_len - used_len,
				    "inst[%lld]:\n ", ctx->id);
		used_len += curr_len;

		if (dbgfs_index[AL_VDEC_DBGFS_PICINFO]) {
			curr_len = snprintf(buf + used_len,
					    total_len - used_len,
					    "\tOUTPUT: %dx%d\n", frame->width,
					    frame->height);
			used_len += curr_len;
			frame = &ctx->dst;

			curr_len = snprintf(buf + used_len,
					    total_len - used_len,
					    "\tCAPTURE: %dx%d\n", frame->width,
					    frame->height);
			used_len += curr_len;
		}

		if (dbgfs_index[AL_VDEC_DBGFS_FORMAT])
			al_vdec_dbgfs_get_format_type(ctx, buf, &used_len,
						      total_len);
	}
	mutex_unlock(&dbgfs->lock);
read_buffer:
	ret = simple_read_from_buffer(ubuf, count, ppos, buf, used_len);
	kfree(buf);
	return ret;
}

static const struct file_operations vdec_fops = {
	.open = simple_open,
	.write = al_vdec_dbgfs_write,
	.read = al_vdec_dbgfs_read,
};

void al_codec_dbgfs_create(struct al_dec_ctx *ctx)
{
	struct al_codec_dbgfs_ctx *dbgfs_ctx;
	struct al_codec_dev *codec = ctx->codec;

	dbgfs_ctx = kzalloc(sizeof(*dbgfs_ctx), GFP_KERNEL);
	if (!dbgfs_ctx)
		return;

	list_add_tail(&dbgfs_ctx->node, &codec->dbgfs.dbgfs_head);

	codec->dbgfs.ctx_count++;

	dbgfs_ctx->ctx_id = ctx->id;
	dbgfs_ctx->dec_ctx = ctx;
}

void al_codec_dbgfs_remove(struct al_codec_dev *codec, int ctx_id)
{
	struct al_codec_dbgfs_ctx *dbgfs_ctx;

	list_for_each_entry(dbgfs_ctx, &codec->dbgfs.dbgfs_head, node) {
		if (dbgfs_ctx->ctx_id == ctx_id) {
			codec->dbgfs.ctx_count--;
			list_del(&dbgfs_ctx->node);
			kfree(dbgfs_ctx);
			return;
		}
	}
}

static void al_codec_dbgfs_vdec_init(struct al_codec_dev *codec)
{
	struct dentry *vcodec_root;

	codec->dbgfs.vcodec_root = debugfs_create_dir("al-vdec", NULL);
	if (IS_ERR(codec->dbgfs.vcodec_root))
		dev_err(&codec->pdev->dev, "create al-vdec dir err:%ld\n",
			PTR_ERR(codec->dbgfs.vcodec_root));

	vcodec_root = codec->dbgfs.vcodec_root;
	debugfs_create_x32("al_v4l2_dbg_level", 0644, vcodec_root,
			   &al_v4l2_dbg_level);
	debugfs_create_x32("al_codec_dbg", 0644, vcodec_root, &al_codec_dbg);

	codec->dbgfs.ctx_count = 0;
	INIT_LIST_HEAD(&codec->dbgfs.dbgfs_head);
	debugfs_create_file("vdec", 0200, vcodec_root, codec, &vdec_fops);
	mutex_init(&codec->dbgfs.lock);
}

void al_codec_dbgfs_init(void *codec)
{
	al_codec_dbgfs_vdec_init(codec);
}

void al_codec_dbgfs_deinit(struct al_codec_dbgfs *dbgfs)
{
	debugfs_remove_recursive(dbgfs->vcodec_root);
}
