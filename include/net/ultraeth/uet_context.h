/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */

#ifndef _UET_CONTEXT_H
#define _UET_CONTEXT_H

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/wait.h>
#include <net/ultraeth/uet_job.h>

struct uet_context {
	int id;
	refcount_t refcnt;
	wait_queue_head_t refcnt_wait;
	struct list_head list;

	struct uet_job_registry job_reg;
};

struct uet_context *uet_context_get_by_id(int id);
void uet_context_put(struct uet_context *ses_pl);

int uet_context_create(int id);
bool uet_context_destroy(int id);
void uet_context_destroy_all(void);

#endif /* _UET_CONTEXT_H */
