/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_COMPLETION_CHAIN_H
#define _LINUX_COMPLETION_CHAIN_H

#include <linux/list.h>
#include <linux/completion.h>
#include <linux/spinlock.h>

struct compl_chain {
	spinlock_t lock;
	struct list_head list;
};

#define COMPL_CHAIN_INIT(name) \
	{ .lock = __SPIN_LOCK_UNLOCKED((name).lock), \
	  .list = LIST_HEAD_INIT((name).list) }

#define DEFINE_COMPL_CHAIN(name) \
	struct compl_chain name = COMPL_CHAIN_INIT(name)

struct compl_chain_entry {
	struct compl_chain *chain;
	struct list_head list;
	struct completion prev_finished;
};

void compl_chain_init(struct compl_chain *chain);
void compl_chain_add(struct compl_chain *chain,
			struct compl_chain_entry *entry);
void compl_chain_wait(struct compl_chain_entry *entry);
void compl_chain_complete(struct compl_chain_entry *entry);
bool compl_chain_pending(struct compl_chain_entry *entry);
void compl_chain_flush(struct compl_chain *chain);

#endif /* _LINUX_COMPLETION_CHAIN_H */
