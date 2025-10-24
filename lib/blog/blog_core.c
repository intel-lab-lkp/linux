// SPDX-License-Identifier: GPL-2.0
/*
 * Binary Logging Infrastructure - Core Implementation
 *
 * Migrated from ceph_san_logger.c with algorithms preserved
 * Client ID management removed - modules handle their own mappings
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <linux/time.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/atomic.h>

#include <linux/blog/blog.h>
#include <linux/blog/blog_batch.h>
#include <linux/blog/blog_pagefrag.h>
#include <linux/blog/blog_ser.h>
#include <linux/blog/blog_des.h>
#include <linux/blog/blog_module.h>

static void blog_tls_release_verbose(void *ptr);
#define NULL_STR "(NULL)"
#define BLOG_LOG_BATCH_MAX_FULL 16

/* Core BLOG functions - all require a valid logger parameter */

/**
 * blog_is_valid_kernel_addr - Check if address is in valid kernel address range
 * @addr: Address to check
 *
 * Returns true if address is in valid kernel address range
 */
bool blog_is_valid_kernel_addr(const void *addr)
{
	if (virt_addr_valid(addr))
		return true;
	return false;
}
EXPORT_SYMBOL(blog_is_valid_kernel_addr);

/**
 * get_context_id - Get a unique context ID
 * @logger: Logger instance to use
 *
 * Acquires a unique ID for a TLS context using the logger's counter
 *
 * Returns a unique context ID
 */
static u64 get_context_id(struct blog_logger *logger)
{
	u64 id;

	spin_lock(&logger->ctx_id_lock);
	id = logger->next_ctx_id++;
	spin_unlock(&logger->ctx_id_lock);
	return id;
}

/**
 * validate_tls_ctx - Validate a TLS context
 * @ctx: Context to validate
 *
 * Returns true if context is valid, false otherwise
 */
static inline bool validate_tls_ctx(struct blog_tls_ctx *ctx)
{
	if (!ctx)
		return false;

#if BLOG_DEBUG_POISON
	if (ctx->debug_poison != BLOG_CTX_POISON) {
		pr_err("BUG: TLS context id=%llu (%llx) has invalid debug_poison value 0x%llx\n",
		       ctx->id, (unsigned long long)ctx,
		       (unsigned long long)ctx->debug_poison);
		return false;
	}
#endif

	if (atomic_read(&ctx->refcount) != 1) {
		pr_err("BUG: TLS context id=%llu (%llx) refcount %d, expected 1\n",
		       ctx->id, (unsigned long long)ctx,
		       atomic_read(&ctx->refcount));
		return false;
	}

	return true;
}

/**
 * add_context_to_global_list - Add a context to the global list
 * @ctx: The context to add to the global list
 *
 * Adds the context to the global list of contexts and updates stats
 */
static void add_context_to_logger_list(struct blog_logger *logger, struct blog_tls_ctx *ctx)
{
	if (!logger)
		return;

	spin_lock(&logger->lock);
	list_add(&ctx->list, &logger->contexts);
	logger->total_contexts_allocated++;
	spin_unlock(&logger->lock);
}

static void remove_context_from_logger_list(struct blog_logger *logger,
					      struct blog_tls_ctx *ctx)
{
	if (!logger)
		return;

	spin_lock(&logger->lock);
	if (!list_empty(&ctx->list)) {
		list_del_init(&ctx->list);
		if (logger->total_contexts_allocated)
			logger->total_contexts_allocated--;
	}
	spin_unlock(&logger->lock);
}

static void blog_tls_clear_task(struct blog_tls_ctx *ctx)
{
	struct task_struct *task;

	if (!ctx)
		return;

	task = READ_ONCE(ctx->task);
	if (task) {
		WRITE_ONCE(ctx->task, NULL);
		put_task_struct(task);
	}
}

static inline struct blog_tls_ctx *get_new_ctx(struct blog_logger *logger)
{
	struct blog_tls_pagefrag *composite;
	struct blog_tls_ctx *ctx;
	struct blog_pagefrag *pf;
	bool is_new = false;

	if (!logger)
		return NULL;

	/* Pop a composite from the alloc_batch */
	composite = blog_batch_get(&logger->alloc_batch);
	if (!composite) {
		/* If batch is empty, allocate a new composite via page allocator (512KB) */
		struct page *pages;

		pages = alloc_pages(GFP_KERNEL | __GFP_ZERO,
				    get_order(BLOG_TLS_PAGEFRAG_ALLOC_SIZE));
		if (!pages) {
			pr_err("%s: Failed to allocate composite via alloc_pages\n", __func__);
			return NULL;
		}
		composite = page_address(pages);
		is_new = true;
	}

	ctx = &composite->ctx;

	/* Check if this composite needs initialization (new or first use from batch) */
	if (is_new || ctx->id == 0) {
		/* Initialize context fields for new composite or uninitialized one */
		INIT_LIST_HEAD(&ctx->list);
		atomic_set(&ctx->refcount, 0);
		ctx->id = get_context_id(logger);

#if BLOG_DEBUG_POISON
		ctx->debug_poison = BLOG_CTX_POISON;
#endif

		pr_debug("[%d]blog: initialized new composite id=%llu (%llx)\n",
			 smp_processor_id(), ctx->id, (unsigned long long)ctx);
	}

	/* Always refresh these fields on reuse */
	ctx->logger = logger;
	ctx->release = blog_tls_release_verbose;

#if BLOG_DEBUG_POISON
	/* Verify debug poison */
	if (ctx->debug_poison != BLOG_CTX_POISON) {
		pr_err("%s: Context id=%llu has invalid debug_poison value 0x%llx\n",
		       __func__, ctx->id, (unsigned long long)ctx->debug_poison);
		WARN_ON_ONCE(1);
		return NULL;
	}
#endif

	/* Initialize the embedded pagefrag to point to inline buffer */
	pf = &composite->pf;
	pf->pages = NULL;  /* No separate pages, using inline buffer */
	pf->buffer = composite->buf;  /* Point to inline buffer */
	pf->capacity = BLOG_TLS_PAGEFRAG_BUFFER_SIZE;  /* Inline buffer size (512KB - header) */
	spin_lock_init(&pf->lock);
	pf->head = 0;
	pf->alloc_count = 0;
	pf->active_elements = 0;
	pf->last_entry = NULL;

	/* Debug: Write buffer offset markers */
	{
		size_t expected_offset = sizeof(struct blog_tls_ctx) + sizeof(struct blog_pagefrag);
		size_t actual_offset = (char *)composite->buf - (char *)composite;
		*(u64 *)composite->buf = expected_offset;
		pr_err("blog: composite=%p, ctx=%p, pf=%p, buf=%p, expected_offset=%zu, actual_offset=%zu\n",
			composite, &composite->ctx, &composite->pf, composite->buf,
			expected_offset, actual_offset);
	}

	/* Add to logger's context list */
	add_context_to_logger_list(logger, ctx);

	/* Set base timestamp */
	ctx->base_jiffies = jiffies;

	blog_logger_print_stats(logger);
	return ctx; /* Context returned with refcount = 0 */
}

/**
 * is_valid_active_ctx - Validate an active TLS context
 * @ctx: Context to validate
 * @context_description: String describing the context for error messages
 *
 * Returns true if context is valid (poison OK, refcount == 1), false otherwise
 */
static inline bool is_valid_active_ctx(struct blog_tls_ctx *ctx,
				       const char *context_description)
{
	if (!ctx) {
		pr_err("BUG: %s context is NULL.\n", context_description);
		return false;
	}

#if BLOG_DEBUG_POISON
	if (ctx->debug_poison != BLOG_CTX_POISON) {
		pr_err("BUG: %s context id=%llu (%llx) has invalid debug_poison value 0x%llx\n",
		       context_description, ctx->id, (unsigned long long)ctx,
		       (unsigned long long)ctx->debug_poison);
		return false;
	}
#endif

	if (atomic_read(&ctx->refcount) != 1) {
		pr_err("BUG: %s context id=%llu (%llx) refcount %d, expected 1\n",
		       context_description, ctx->id, (unsigned long long)ctx,
		       atomic_read(&ctx->refcount));
		return false;
	}
	return true;
}

/* Release function for TLS storage */
static void blog_tls_release(void *ptr)
{
	struct blog_tls_ctx *ctx = ptr;
	struct blog_tls_pagefrag *composite;

	if (!ctx)
		return;

	if (atomic_dec_return(&ctx->refcount) != 0) {
		pr_err("BUG: TLS context id=%llu refcount %d after release\n",
		       ctx->id, atomic_read(&ctx->refcount));
		panic("blog: TLS context id=%llu refcount %d after release\n",
		      ctx->id, atomic_read(&ctx->refcount));
	}
	pr_debug("blog: decremented refcount=0 for context id=%llu\n", ctx->id);

	/* Clear task association */
	blog_tls_clear_task(ctx);
	pr_debug("blog: releasing TLS context for pid %d [%s]\n", ctx->pid,
		 ctx->comm);

	if (ctx->logger) {
		/* Remove from logger's context list */
		remove_context_from_logger_list(ctx->logger, ctx);

		/* Recycle composite to log_batch - logs remain readable!
		 * Composite will flow: log_batch → drain → reset → alloc_batch */
		composite = blog_ctx_container(ctx);
		blog_batch_put(&ctx->logger->log_batch, composite);

		/* If log_batch has too many full magazines, move one to alloc_batch */
		if (ctx->logger->log_batch.nr_full > BLOG_LOG_BATCH_MAX_FULL) {
			struct blog_magazine *mag;

			spin_lock(&ctx->logger->log_batch.full_lock);
			if (!list_empty(&ctx->logger->log_batch.full_magazines)) {
				mag = list_first_entry(
					&ctx->logger->log_batch.full_magazines,
					struct blog_magazine, list);
				list_del(&mag->list);
				ctx->logger->log_batch.nr_full--;
				spin_unlock(&ctx->logger->log_batch.full_lock);

				spin_lock(&ctx->logger->alloc_batch.full_lock);
				list_add(&mag->list,
					 &ctx->logger->alloc_batch.full_magazines);
				ctx->logger->alloc_batch.nr_full++;
				spin_unlock(&ctx->logger->alloc_batch.full_lock);
			} else {
				spin_unlock(&ctx->logger->log_batch.full_lock);
			}
		}
	} else {
		pr_err("BUG: TLS context id=%llu has no logger for batch release\n",
		       ctx->id);
	}
}

static void blog_tls_release_verbose(void *ptr)
{
	struct blog_tls_ctx *ctx = (struct blog_tls_ctx *)ptr;

	if (!ctx) {
		pr_err("blog -- Callback : invalid TLS context pointer %d\n",
		       current->pid);
		return;
	}
#if BLOG_DEBUG_POISON
	if (ctx->debug_poison != BLOG_CTX_POISON) {
		pr_err("%s: invalid TLS context id=%llu has invalid debug_poison value 0x%llx\n",
		       __func__, ctx->id, (unsigned long long)ctx->debug_poison);
		WARN_ON_ONCE(1);
		return;
	}
#endif
	if (atomic_read(&ctx->refcount) != 1) {
		pr_err("%s: invalid TLS context refcount %d for pid %d [%s]\n",
		       __func__, atomic_read(&ctx->refcount), ctx->pid, ctx->comm);
		WARN_ON_ONCE(1);
		return;
	}
	blog_tls_release(ctx);
}

static struct blog_tls_ctx *lookup_active_ctx(struct blog_logger *logger)
{
	struct blog_tls_ctx *ctx;

	if (!logger)
		return NULL;

	if (logger->has_slot) {
		u8 slot = logger->slot_id;
		struct blog_tls_ctx *slot_ctx;

		if (slot < BLOG_MAX_MODULES) {
			slot_ctx = READ_ONCE(current->blog_contexts[slot]);
			if (slot_ctx)
				return slot_ctx;
		}
	}

	spin_lock(&logger->lock);
	list_for_each_entry(ctx, &logger->contexts, list) {
		if (ctx->task == current) {
			spin_unlock(&logger->lock);
			return ctx;
		}
	}
	spin_unlock(&logger->lock);
	return NULL;
}

/**
 * blog_get_tls_ctx - Get or create TLS context for current task
 * @logger: Logger instance to use
 *
 * Returns pointer to TLS context or NULL on error
 */
struct blog_tls_ctx *blog_get_tls_ctx(struct blog_logger *logger)
{
	struct blog_tls_ctx *ctx = lookup_active_ctx(logger);

	/* Context already exists - handled by slot-based system */
	if (ctx)
		return ctx;

	if (logger && logger->has_slot && logger->owner_ctx)
		return blog_get_tls_ctx_ctx(logger->owner_ctx);

	/* Create new context */
	pr_debug("blog: creating new TLS context for pid %d [%s]\n",
		 current->pid, current->comm);

	ctx = get_new_ctx(logger);
	if (!ctx)
		return NULL;

	blog_tls_clear_task(ctx);
	get_task_struct(current);

	/* Set up TLS specific parts */
	/* Note: slot-based storage is handled by blog_module layer */
	ctx->task = current;
	ctx->pid = current->pid;
	strscpy(ctx->comm, current->comm, TASK_COMM_LEN);

	/* Increment refcount from 0 to 1 */
	if (atomic_inc_return(&ctx->refcount) != 1) {
		pr_err("%s: Failed to set refcount=1 for new TLS context id=%llu (was %d before inc)\n",
		       __func__, ctx->id, atomic_read(&ctx->refcount) - 1);
		WARN_ON_ONCE(1);
	}

	pr_debug(
		"blog: successfully created new TLS context id=%llu for pid %d [%s]\n",
		ctx->id, ctx->pid, ctx->comm);
	return ctx;
}
EXPORT_SYMBOL(blog_get_tls_ctx);

/**
 * blog_get_source_id - Get or create a source ID for the given location
 * @logger: Logger instance to use (NULL for global)
 * @file: Source file name
 * @func: Function name
 * @line: Line number
 * @fmt: Format string
 *
 * Returns a unique ID for this source location
 */
u32 blog_get_source_id(struct blog_logger *logger, const char *file,
		       const char *func, unsigned int line, const char *fmt)
{
	u32 id;

	if (!logger)
		return 0;

	id = atomic_inc_return(&logger->next_source_id);

	if (id >= BLOG_MAX_SOURCE_IDS) {
		/* If we run out of IDs, just use the first one */
		pr_warn("blog: source ID overflow, reusing ID 1\n");
		id = 1;
	}

	/* Store the source information in the logger's map */
	logger->source_map[id].file = file;
	logger->source_map[id].func = func;
	logger->source_map[id].line = line;
	logger->source_map[id].fmt = fmt;
	logger->source_map[id].warn_count = 0;

	pr_err("blog_get_source_id: id=%u, file=%s, func=%s, line=%u, fmt=%p (%s)\n",
		id, file, func, line, fmt, fmt ? fmt : "NULL");

	return id;
}
EXPORT_SYMBOL(blog_get_source_id);

/**
 * blog_get_source_info - Get source info for a given ID
 * @id: Source ID
 *
 * Returns the source information for this ID
 */
struct blog_source_info *blog_get_source_info(struct blog_logger *logger, u32 id)
{
	if (!logger || unlikely(id == 0 || id >= BLOG_MAX_SOURCE_IDS))
		return NULL;
	return &logger->source_map[id];
}
EXPORT_SYMBOL(blog_get_source_info);

/**
 * blog_log - Log a message
 * @source_id: Source ID for this location
 * @client_id: Client ID for this message (module-specific)
 * @needed_size: Size needed for the message
 *
 * Returns a buffer to write the message into
 */
void *blog_log(struct blog_logger *logger, u32 source_id, u8 client_id, size_t needed_size)
{
	struct blog_tls_ctx *ctx;
	struct blog_log_entry *entry = NULL;
	int alloc;
	int retry_count = 0;

#if BLOG_TRACK_USAGE
	struct blog_source_info *source;
#endif
	/* Preserve payload length; compute rounded total allocation separately */
	size_t payload_len = needed_size;

	if (payload_len > BLOG_MAX_PAYLOAD) {
		pr_warn_once("%s: payload %zu exceeds max %u\n",
				__func__, payload_len, BLOG_MAX_PAYLOAD);
		return NULL;
	}

	needed_size = round_up(payload_len + sizeof(struct blog_log_entry), 8);
#if BLOG_TRACK_USAGE
	/* Get source info to update stats */
	source = blog_get_source_info(logger, source_id);
	if (unlikely(source)) {
		if (in_serving_softirq()) {
			atomic_inc(&source->napi_usage);
			atomic_add(needed_size, &source->napi_bytes);
		} else {
			atomic_inc(&source->task_usage);
			atomic_add(needed_size, &source->task_bytes);
		}
	}
#endif

	while (entry == NULL) {
		struct blog_pagefrag *pf;

		ctx = blog_get_ctx(logger);
		if (!ctx) {
			pr_err("Failed to get TLS context\n");
			return NULL;
		}
		if (!blog_is_valid_kernel_addr(ctx)) {
			pr_err("%s: invalid TLS context address: %pK\n",
			       __func__, ctx);
			return NULL;
		}
		if (unlikely(retry_count)) {
			pr_debug(
				"[%d]Retrying allocation with ctx %llu (%s, pid %d) (retry %d, needed_size=%zu @ %d)\n",
				smp_processor_id(), ctx->id, ctx->comm,
				ctx->pid, retry_count, needed_size, source_id);
		}

		pf = blog_ctx_pf(ctx);
		alloc = blog_pagefrag_reserve(pf, needed_size);
		if (alloc == -ENOMEM) {
			pr_debug("%s: allocation failed (needed %zu), resetting context\n",
				 __func__, needed_size);
			blog_pagefrag_reset(pf);
			retry_count++;
			if (retry_count > 3) {
				pr_err("%s: failed to allocate after 3 retries\n", __func__);
				return NULL;
			}
			continue;
		}

		entry = blog_pagefrag_get_ptr(pf, alloc);
		if (!entry) {
			pr_err("%s: failed to get pointer from pagefrag\n", __func__);
			return NULL;
		}
		pf->last_entry = entry;

		/* Store pending publish info for blog_log_commit() */
		ctx->pending_offset = alloc;
		ctx->pending_size = needed_size;
	}

#if BLOG_DEBUG_POISON
	entry->debug_poison = BLOG_LOG_ENTRY_POISON;
#endif
	entry->ts_delta = (u32)(jiffies - ctx->base_jiffies);
	entry->source_id = (u16)source_id;
	entry->len = (u8)payload_len;
	entry->client_id = client_id;
	entry->flags = 0;

	pr_err("blog_log: source_id=%u, payload_len=%zu, needed_size=%zu, offset=%d\n",
		source_id, payload_len, needed_size, ctx->pending_offset);

	/* Debug: Show entry location relative to composite */
	{
		struct blog_tls_pagefrag *composite = blog_ctx_container(ctx);
		size_t entry_offset_from_composite = (char *)entry - (char *)composite;
		size_t buffer_offset_from_composite = (char *)composite->buf - (char *)composite;
		pr_err("blog_log: composite=%p, entry=%p, entry_offset=%zu, buffer_start_offset=%zu\n",
			composite, entry, entry_offset_from_composite, buffer_offset_from_composite);
	}

	return entry->buffer;
}
EXPORT_SYMBOL(blog_log);

/**
 * blog_log_commit - Publish a reserved log entry
 * @logger: Logger instance
 * @actual_size: Actual bytes written during serialization
 *
 * Publishes the log entry that was reserved by the last blog_log() call.
 * Must be called after serialization is complete to make the entry visible
 * to readers.
 *
 * Context: Same context as the preceding blog_log() call
 * Return: 0 on success, negative error code on failure
 */
int blog_log_commit(struct blog_logger *logger, size_t actual_size)
{
	struct blog_tls_ctx *ctx = blog_get_ctx(logger);
	struct blog_pagefrag *pf;
	struct blog_log_entry *entry;
	size_t total_size;

	if (!ctx)
		return -EINVAL;

	pf = blog_ctx_pf(ctx);

	/* Get the entry we're committing and update its length to actual size */
	entry = blog_pagefrag_get_ptr(pf, ctx->pending_offset);
	if (entry)
		entry->len = (u8)actual_size;

	/* actual_size is payload only, need to add header + alignment */
	total_size = round_up(sizeof(struct blog_log_entry) + actual_size, 8);

	pr_err("blog_log_commit: pending_offset=%d, actual_size=%zu, total_size=%zu, pending_size=%zu\n",
		ctx->pending_offset, actual_size, total_size, ctx->pending_size);

	blog_pagefrag_publish(pf, ctx->pending_offset + total_size);

	return 0;
}
EXPORT_SYMBOL(blog_log_commit);

/**
 * blog_get_napi_ctx - Get NAPI context for current CPU
 */
struct blog_tls_ctx *blog_get_napi_ctx(struct blog_logger *logger)
{
	struct blog_tls_ctx **napi_ctx_ptr;

	if (!logger || !logger->napi_ctxs)
		return NULL;

	napi_ctx_ptr = per_cpu_ptr(logger->napi_ctxs, smp_processor_id());
	return napi_ctx_ptr ? *napi_ctx_ptr : NULL;
}
EXPORT_SYMBOL(blog_get_napi_ctx);

/**
 * blog_set_napi_ctx - Set NAPI context for current CPU
 */
void blog_set_napi_ctx(struct blog_logger *logger, struct blog_tls_ctx *ctx)
{
	struct blog_tls_ctx **napi_ctx_ptr;

	if (!logger || !logger->napi_ctxs)
		return;

	napi_ctx_ptr = per_cpu_ptr(logger->napi_ctxs, smp_processor_id());
	if (napi_ctx_ptr)
		*napi_ctx_ptr = ctx;
}
EXPORT_SYMBOL(blog_set_napi_ctx);

/**
 * blog_get_ctx - Get appropriate context based on context type
 */
struct blog_tls_ctx *blog_get_ctx(struct blog_logger *logger)
{
	if (in_serving_softirq()) {
		struct blog_tls_ctx *n = blog_get_napi_ctx(logger);

		if (n)
			return n;
		/* Fallback to TLS context if no NAPI context set */
	}
	return blog_get_tls_ctx(logger);
}
EXPORT_SYMBOL(blog_get_ctx);

/**
 * blog_log_iter_init - Initialize the iterator for a specific pagefrag
 */
void blog_log_iter_init(struct blog_log_iter *iter, struct blog_pagefrag *pf,
			u64 head_snapshot)
{
	if (!iter || !pf)
		return;

	iter->pf = pf;
	iter->current_offset = 0;
	iter->end_offset = head_snapshot;
	iter->prev_offset = 0;
	iter->steps = 0;
}
EXPORT_SYMBOL(blog_log_iter_init);

/**
 * blog_log_iter_next - Get next log entry
 */
struct blog_log_entry *blog_log_iter_next(struct blog_log_iter *iter)
{
	struct blog_log_entry *entry;

	if (!iter || iter->current_offset >= iter->end_offset)
		return NULL;

	entry = blog_pagefrag_get_ptr(iter->pf, iter->current_offset);
	if (!entry)
		return NULL;

	iter->prev_offset = iter->current_offset;
	iter->current_offset +=
		round_up(sizeof(struct blog_log_entry) + entry->len, 8);
	iter->steps++;

	return entry;
}
EXPORT_SYMBOL(blog_log_iter_next);

/**
 * blog_des_entry - Deserialize entry with callback
 */
int blog_des_entry(struct blog_logger *logger, struct blog_log_entry *entry,
		   char *output, size_t out_size, blog_client_des_fn client_cb)
{
	int len = 0;
	struct blog_source_info *source;

	if (!entry || !output)
		return -EINVAL;

	/* Let module handle client_id if callback provided */
	if (client_cb) {
		len = client_cb(output, out_size, entry->client_id);
		if (len < 0)
			return len;
	}

	/* Get source info */
	source = blog_get_source_info(logger, entry->source_id);
	if (!source) {
		len += snprintf(output + len, out_size - len,
				"[unknown source %u]", entry->source_id);
		return len;
	}

	pr_err("blog_des_entry: source_id=%u, source=%p, fmt=%p (%s), entry->len=%u\n",
		entry->source_id, source, source->fmt, source->fmt ? source->fmt : "NULL", entry->len);

	/* Debug: Show entry pointer and payload pointer */
	pr_err("blog_des_entry: entry=%p, entry->buffer=%p, buffer_offset=%ld\n",
		entry, entry->buffer, (char *)entry->buffer - (char *)entry);

	/* Add source location */
	len += snprintf(output + len, out_size - len, "[%s:%s:%u] ",
			source->file, source->func, source->line);

	/* Deserialize the buffer content */
	len += blog_des_reconstruct(source->fmt, entry->buffer, 0, entry->len,
				    output + len, out_size - len);

	return len;
}
EXPORT_SYMBOL(blog_des_entry);

/* No global init/exit: consumers initialize per‑module contexts explicitly */

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Binary Logging Infrastructure (BLOG)");
