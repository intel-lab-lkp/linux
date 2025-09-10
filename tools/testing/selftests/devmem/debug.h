/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * devmem test debug.h
 *
 * Copyright (C) 2025 Red Hat, Inc. All Rights Reserved.
 * Written by Alessandro Carminati (acarmina@redhat.com)
 */

#ifndef DEBUG_H
#define DEBUG_H
extern int pdebug;
void deb_printf(const char *fmt, ...);
#endif

