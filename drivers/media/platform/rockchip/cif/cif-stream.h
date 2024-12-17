/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Rockchip Camera Interface (CIF) Driver
 *
 * Copyright (C) 2024 Michael Riesch <michael.riesch@wolfvision.net>
 */

#ifndef _CIF_STREAM_H
#define _CIF_STREAM_H

#include "cif-common.h"

struct cif_stream_config {
	const char *name;
};

void cif_stream_pingpong(struct cif_stream *stream);

int cif_stream_register(struct cif_device *cif_dev, struct cif_stream *stream,
			const struct cif_stream_config *config);

void cif_stream_unregister(struct cif_stream *stream);

#endif
