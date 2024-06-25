// SPDX-License-Identifier: GPL-2.0
#include <opencsd/c_api/opencsd_c_api.h>
#include "cs-etm-decoder/cs-etm-min-version.h"

/*
 * Check OpenCSD library version is sufficient to provide required features
 */
#if !defined(OCSD_VER_NUM) || (OCSD_VER_NUM < OCSD_MIN_VER)
#error "OpenCSD minimum version (OCSD_MIN_VER) not met."
#endif

int main(void)
{
	(void)ocsd_get_version();
	return 0;
}
