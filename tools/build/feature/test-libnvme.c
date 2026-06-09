// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include <libnvme.h>

int main(void)
{
	nvme_root_t r = nvme_scan(NULL);

	if (r)
		nvme_free_tree(r);
	return 0;
}
