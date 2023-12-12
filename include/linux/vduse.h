/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VDUSE_H
#define _LINUX_VDUSE_H

/*
 * The permission required for a VDUSE device operation.
 */
enum vduse_op_perm {
	VDUSE_PERM_CREATE,
	VDUSE_PERM_DESTROY,
	VDUSE_PERM_OPEN,
};

#endif /* _LINUX_VDUSE_H */
