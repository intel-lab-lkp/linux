// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include "header.h"

/**
 * cpc_header_get_seq() - Get the sequence number.
 * @hdr: CPC header.
 *
 * Return: Sequence number.
 */
u8 cpc_header_get_seq(const struct cpc_header *hdr)
{
	return hdr->seq;
}
