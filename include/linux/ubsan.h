/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UBSAN_H
#define _LINUX_UBSAN_H

#if defined(CONFIG_UBSAN_TRAP) || defined(CONFIG_UBSAN_KVM_EL2) || \
    defined(CONFIG_OVERFLOW_BEHAVIOR_TYPES_TRAP)
# define NEED_SANITIZER_TRAP_HANDLER
#endif

#if (defined(CONFIG_UBSAN) && !defined(CONFIG_UBSAN_TRAP)) || \
    defined(CONFIG_OVERFLOW_BEHAVIOR_TYPES_WARN)
# define NEED_SANITIZER_WARN_HANDLER
#endif

#ifdef NEED_SANITIZER_TRAP_HANDLER
const char *report_ubsan_failure(u32 check_type);
#else
static inline const char *report_ubsan_failure(u32 check_type)
{
	return NULL;
}
#endif

#endif
