// SPDX-License-Identifier: GPL-2.0
#include "util/evsel.h"

int arch_evsel__hw_name(struct evsel *evsel, char *bf, size_t size)
{
	return evsel__hw_name_ext_type_id(evsel, bf, size);
}
