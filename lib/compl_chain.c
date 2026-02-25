// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Maurizio Lombardi <mlombard@redhat.com>
 */
#include <linux/compl_chain.h>

/**
 * compl_chain_init - Initialize a completion chain
 * @chain: The completion chain to be initialized.
 *
 * Initializes a compl_chain structure
 */
void compl_chain_init(struct compl_chain *chain)
{
	spin_lock_init(&chain->lock);
	INIT_LIST_HEAD(&chain->list);
}
EXPORT_SYMBOL_GPL(compl_chain_init);

/**
 * compl_chain_add - Add a new entry to the tail of the chain
 * @chain: The completion chain to add the entry to.
 * @entry: The entry to be enqueued.
 *
 * Adds a new entry to the end of the queue.
 * If the chain is empty when this entry is added, it is immediately marked
 * as ready to run, as there is no preceding entry to wait for.
 */
void compl_chain_add(struct compl_chain *chain,
			struct compl_chain_entry *entry)
{
	init_completion(&entry->prev_finished);
	INIT_LIST_HEAD(&entry->list);

	WRITE_ONCE(entry->chain, chain);

	spin_lock(&chain->lock);
	if (list_empty(&chain->list))
		complete_all(&entry->prev_finished);
	list_add_tail(&entry->list, &chain->list);
	spin_unlock(&chain->lock);
}
EXPORT_SYMBOL_GPL(compl_chain_add);

/**
 * compl_chain_wait - Wait for the preceding operation to finish
 * @entry: The entry for the current operation.
 *
 * Blocks the current execution thread until compl_chain_complete()
 * is executed against the previous entry in the chain.
 */
void compl_chain_wait(struct compl_chain_entry *entry)
{
	WARN_ON(!entry->chain);

	wait_for_completion(&entry->prev_finished);
}
EXPORT_SYMBOL_GPL(compl_chain_wait);

/**
 * compl_chain_complete - Mark an entry as completed and signal the next one
 * @entry: The entry to mark as completed.
 *
 * Removes the current entry from the chain and signals the next waiting
 * entry (if one exists) that it is now allowed to proceed.
 */
void compl_chain_complete(struct compl_chain_entry *entry)
{
	struct compl_chain *chain = entry->chain;

	WARN_ON(!chain);

	wait_for_completion(&entry->prev_finished);

	spin_lock(&chain->lock);
	list_del(&entry->list);
	if (!list_empty(&chain->list)) {
		struct compl_chain_entry *next =
			list_first_entry(&chain->list,
					 struct compl_chain_entry, list);
		complete_all(&next->prev_finished);
	}
	spin_unlock(&chain->lock);

	WRITE_ONCE(entry->chain, NULL);
}
EXPORT_SYMBOL_GPL(compl_chain_complete);

/**
 * compl_chain_pending - Check if an entry is pending
 * @entry: The entry to check.
 *
 * Returns true if an entry has been added to a chain and hasn't yet
 * been completed.
 */
bool compl_chain_pending(struct compl_chain_entry *entry)
{
	return READ_ONCE(entry->chain) != NULL;
}
EXPORT_SYMBOL_GPL(compl_chain_pending);

/**
 * compl_chain_flush - Wait for all entries currently in the chain to finish
 * @chain: The completion chain to flush.
 *
 * Enqueues a dummy entry into the chain and immediately calls
 * compl_chain_complete() against it. Because operations execute in strict
 * FIFO order, this acts as a barrier, blocking the calling thread until
 * all previously enqueued entries have finished.
 */
void compl_chain_flush(struct compl_chain *chain)
{
	struct compl_chain_entry dummy_entry;

	compl_chain_add(chain, &dummy_entry);
	compl_chain_complete(&dummy_entry);
}
EXPORT_SYMBOL_GPL(compl_chain_flush);
