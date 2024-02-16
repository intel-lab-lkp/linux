/* SPDX-License-Identifier: GPL-2.0 */
/* PGP crypto data parser internal definitions
 *
 * Copyright (C) 2011 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#include "pgplib.h"

#define kenter(FMT, ...) \
	pr_devel("==> %s("FMT")\n", __func__, ##__VA_ARGS__)
#define kleave(FMT, ...) \
	pr_devel("<== %s()"FMT"\n", __func__, ##__VA_ARGS__)

/*
 * pgp_public_key.c
 */
extern const char *pgp_to_public_key_algo[PGP_PUBKEY__LAST];
