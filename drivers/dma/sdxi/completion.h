/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright Advanced Micro Devices, Inc. */
#ifndef DMA_SDXI_COMPLETION_H
#define DMA_SDXI_COMPLETION_H

#include <linux/compiler_attributes.h>
#include "sdxi.h"

/*
 * Polled completion status block that can be attached to a
 * descriptor.
 */
struct sdxi_completion;
struct sdxi_desc;
struct sdxi_completion *sdxi_completion_alloc(struct sdxi_dev *sdxi);
void sdxi_completion_free(struct sdxi_completion *sc);
int __must_check sdxi_completion_poll(const struct sdxi_completion *sc);
void sdxi_completion_attach(struct sdxi_desc *desc,
			    const struct sdxi_completion *sc);
bool sdxi_completion_signaled(const struct sdxi_completion *sc);
bool sdxi_completion_errored(const struct sdxi_completion *sc);

DEFINE_FREE(sdxi_completion, struct sdxi_completion *, if (_T) sdxi_completion_free(_T))

#endif /* DMA_SDXI_COMPLETION_H */
