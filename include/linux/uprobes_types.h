/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _LINUX_UPROBES_TYPES_H
#define _LINUX_UPROBES_TYPES_H
/*
 * User-space Probes (UProbes)
 *
 * Copyright (C) IBM Corporation, 2008-2012
 * Authors:
 *	Srikar Dronamraju
 *	Jim Keniston
 * Copyright (C) 2011-2012 Red Hat, Inc., Peter Zijlstra
 */

#ifdef CONFIG_UPROBES

struct xol_area;

struct uprobes_state {
	struct xol_area		*xol_area;
};
#else /* !CONFIG_UPROBES */
struct uprobes_state {
};
#endif /* !CONFIG_UPROBES */
#endif	/* _LINUX_UPROBES_TYPES_H */
