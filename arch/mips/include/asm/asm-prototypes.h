/* SPDX-License-Identifier: GPL-2.0 */
#include <asm/page.h>
#include <asm/fpu.h>
#include <asm-generic/asm-prototypes.h>
#include <linux/uaccess.h>
#include <asm/ftrace.h>
#include <asm/mmu_context.h>
#include <net/checksum.h>

extern void clear_page_cpu(void *page);
extern void copy_page_cpu(void *to, void *from);
