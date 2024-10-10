/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _LINUX_STACKLOG_DEBUG_H
#define _LINUX_STACKLOG_DEBUG_H

#ifdef CONFIG_STACKLOG_DEBUG
void stacklog_debug_save(void);
#else
static inline void stacklog_debug_save(void)
{
}
#endif
#endif
