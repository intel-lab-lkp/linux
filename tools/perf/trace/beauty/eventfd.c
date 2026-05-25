// SPDX-License-Identifier: LGPL-2.1
#include "trace/beauty/beauty.h"
#include "trace/beauty/include/uapi/linux/eventfd.h"


size_t syscall_arg__scnprintf_eventfd_flags(char *bf, size_t size, struct syscall_arg *arg)
{
	bool show_prefix = arg->show_string_prefix;
	const char *prefix = "EFD_";
	int printed = 0, flags = arg->val;

	if (flags == 0)
		return scnprintf(bf, size, "NONE");
#define	P_FLAG(n) \
	if (flags & EFD_##n) { \
		printed += scnprintf(bf + printed, size - printed, "%s%s%s", printed ? "|" : "", show_prefix ? prefix : "", #n); \
		flags &= ~EFD_##n; \
	}

	P_FLAG(SEMAPHORE);
	P_FLAG(CLOEXEC);
	P_FLAG(NONBLOCK);
#undef P_FLAG

	if (flags)
		printed += scnprintf(bf + printed, size - printed, "%s%#x", printed ? "|" : "", flags);

	return printed;
}
