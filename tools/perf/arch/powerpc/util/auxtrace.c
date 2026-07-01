// SPDX-License-Identifier: GPL-2.0
/*
 * VPA support
 */
#include <errno.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/zalloc.h>

#include "../../util/evlist.h"
#include "../../util/debug.h"
#include "../../util/auxtrace.h"
#include "../../util/powerpc-vpadtl.h"
#include "../../util/record.h"

struct auxtrace_record *auxtrace_record__init(struct evlist *evlist,
						int *err)
{
	struct evsel *pos;
	int found_vpa_dtl = 0;

	/*
	 * Set err value to zero here. Any fail later
	 * will set appropriate return code to err.
	 */
	*err = 0;

	evlist__for_each_entry(evlist, pos) {
		if (strstarts(pos->name, "vpa_dtl")) {
			found_vpa_dtl = 1;
			pos->needs_auxtrace_mmap = true;
			break;
		}
	}

	if (found_vpa_dtl)
		return vpa_dtl_recording_init(pos);
	else {
		*err = -EINVAL;
		return NULL;
	}
}
