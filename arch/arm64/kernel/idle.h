/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ARM64_KERNEL_IDLE_H
#define __ARM64_KERNEL_IDLE_H

extern enum arm64_idle_mode idle;

enum arm64_idle_mode {
	ARM64_IDLE_WFI,
	ARM64_IDLE_YIELD,
	ARM64_IDLE_NOP,
};

#endif
