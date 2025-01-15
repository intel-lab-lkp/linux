/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _TEST_KLP_SPEAKER_H_
#define _TEST_KLP_SPEAKER_H_

#include <linux/workqueue.h>

typedef void (*do_block_doors_t)(void);

struct hall {
	struct work_struct block_doors_work;
	do_block_doors_t do_block_doors;
};

#endif //  _TEST_KLP_SPEAKER_H_
