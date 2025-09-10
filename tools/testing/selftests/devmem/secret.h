/* SPDX-License-Identifier: GPL-2.0+ */
/* devmem test secret.h
 *
 * Copyright (C) 2025 Red Hat, Inc. All Rights Reserved.
 * Written by Alessandro Carminati (acarmina@redhat.com)
 */

#ifndef SECRET_H
#define SECRET_H

void *secret_alloc(size_t size);
void secret_free(void *p, size_t size);
#endif
