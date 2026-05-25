// SPDX-License-Identifier: LGPL-2.1
#include "trace/beauty/beauty.h"
#include "trace/beauty/include/uapi/linux/futex.h"


size_t syscall_arg__scnprintf_futex_val3(char *bf, size_t size, struct syscall_arg *arg)
{
	const char *prefix = "FUTEX_BITSET_";
	unsigned int bitset = arg->val;

	if (bitset == FUTEX_BITSET_MATCH_ANY)
		return scnprintf(bf, size, "%s%s", arg->show_string_prefix ? prefix : "", "MATCH_ANY");

	return scnprintf(bf, size, "%#xd", bitset);
}
