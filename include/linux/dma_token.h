/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_DMA_TOKEN_H
#define _LINUX_DMA_TOKEN_H

#include <linux/dma-buf.h>

struct dma_token_params {
	struct dma_buf			*dmabuf;
	enum dma_data_direction		dir;
};

struct dma_token {
	void (*release)(struct dma_token *);
};

static inline void dma_token_release(struct dma_token *token)
{
	token->release(token);
}

static inline struct dma_token *
dma_token_create(struct file *file, struct dma_token_params *params)
{
	struct dma_token *res;

	if (!file->f_op->dma_map)
		return ERR_PTR(-EOPNOTSUPP);
	res = file->f_op->dma_map(file, params);

	WARN_ON_ONCE(!IS_ERR(res) && !res->release);

	return res;
}

#endif
