// SPDX-License-Identifier: GPL-2.0+
/*
 * devmem test debug.c
 *
 * Copyright (C) 2025 Red Hat, Inc. All Rights Reserved.
 * Written by Alessandro Carminati (acarmina@redhat.com)
 */

#include <stdio.h>
#include <stdarg.h>

#define DEBUG_FLAG 0
int pdebug = DEBUG_FLAG;

void deb_printf(const char *fmt, ...)
{
	va_list args;

	if (pdebug) {
		va_start(args, fmt);
		vprintf(fmt, args);
		va_end(args);
	}
}

