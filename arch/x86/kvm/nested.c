// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kvm_host.h>

static struct kvm_nested_context_table *kvm_nested_context_table_alloc(void)
{
	struct kvm_nested_context_table *table;

	table = kzalloc(sizeof(*table), GFP_KERNEL_ACCOUNT);
	if (!table)
		return NULL;

	spin_lock_init(&table->lock);
	INIT_LIST_HEAD(&table->lru_list);
	hash_init(table->hash);
	return table;
}

static void kvm_nested_context_table_free(struct kvm_nested_context_table
					  *table)
{
	kfree(table);
}

int kvm_nested_context_table_init(struct kvm *kvm)
{
	struct kvm_nested_context_table *table;

	if (!kvm_x86_ops.nested_ops->alloc_context ||
	    !kvm_x86_ops.nested_ops->free_context ||
	    !kvm_x86_ops.nested_ops->reset_context)
		return -EINVAL;

	table = kvm_nested_context_table_alloc();
	if (!table)
		return -ENOMEM;

	kvm->arch.nested_context_table = table;
	return 0;
}

void kvm_nested_context_table_destroy(struct kvm *kvm)
{
	struct kvm_nested_context_table *table;
	struct kvm_nested_context *ctx;
	struct hlist_node *tmp;
	int bkt;

	table = kvm->arch.nested_context_table;
	if (!table)
		return;

	hash_for_each_safe(table->hash, bkt, tmp, ctx, hnode) {
		hash_del(&ctx->hnode);
		kvm_x86_ops.nested_ops->free_context(ctx);
	}

	kvm_nested_context_table_free(table);
}

static unsigned int kvm_nested_context_max(struct kvm *kvm)
{
	return KVM_NESTED_OVERSUB_RATIO * atomic_read(&kvm->online_vcpus);
}

static struct kvm_nested_context *__kvm_nested_context_find(struct kvm_nested_context_table
							    *table, gpa_t gpa)
{
	struct kvm_nested_context *ctx;

	hash_for_each_possible(table->hash, ctx, hnode, gpa) {
		if (ctx->gpa == gpa)
			return ctx;
	}

	return NULL;
}

static struct kvm_nested_context *kvm_nested_context_find(struct
							  kvm_nested_context_table
							  *table,
							  struct kvm_vcpu *vcpu,
							  gpa_t gpa)
{
	struct kvm_nested_context *ctx;

	ctx = __kvm_nested_context_find(table, gpa);
	if (!ctx)
		return NULL;

	WARN_ON_ONCE(ctx->vcpu && ctx->vcpu != vcpu);

	/* Remove from the LRU list if not attached to a vcpu */
	if (!ctx->vcpu)
		list_del(&ctx->lru_link);

	return ctx;
}

static struct kvm_nested_context *kvm_nested_context_recycle(struct
							     kvm_nested_context_table
							     *table)
{
	struct kvm_nested_context *ctx;

	if (list_empty(&table->lru_list))
		return NULL;

	ctx =
	    list_first_entry(&table->lru_list, struct kvm_nested_context,
			     lru_link);
	list_del(&ctx->lru_link);
	hash_del(&ctx->hnode);
	return ctx;
}

static void kvm_nested_context_insert(struct kvm_nested_context_table *table,
				      struct kvm_nested_context *ctx, gpa_t gpa)
{
	hash_add(table->hash, &ctx->hnode, gpa);
	ctx->gpa = gpa;
}

struct kvm_nested_context *kvm_nested_context_load(struct kvm_vcpu *vcpu,
						   gpa_t gpa)
{
	struct kvm_nested_context_table *table;
	struct kvm_nested_context *ctx, *new_ctx = NULL;
	struct kvm *vm = vcpu->kvm;
	bool reset = false;

	table = vcpu->kvm->arch.nested_context_table;
	if (WARN_ON_ONCE(!table))
		return NULL;
retry:
	spin_lock(&table->lock);
	ctx = kvm_nested_context_find(table, vcpu, gpa);
	if (!ctx) {
		/* At capacity? Recycle the LRU context */
		if (table->count >= kvm_nested_context_max(vcpu->kvm)) {
			ctx = kvm_nested_context_recycle(table);
			if (unlikely(!ctx))
				goto finish;

			kvm_nested_context_insert(table, ctx, gpa);
			++vm->stat.nested_context_recycle;
			reset = true;

		} else if (new_ctx) {
			++table->count;
			ctx = new_ctx;
			kvm_nested_context_insert(table, ctx, gpa);
			new_ctx = NULL;

		} else {
			/* Allocate a new context without holding the lock */
			spin_unlock(&table->lock);
			new_ctx = kvm_x86_ops.nested_ops->alloc_context(vcpu);
			if (unlikely(!new_ctx))
				return NULL;

			goto retry;
		}
	} else
		++vm->stat.nested_context_reuse;

	ctx->vcpu = vcpu;
finish:
	spin_unlock(&table->lock);

	if (new_ctx)
		kvm_x86_ops.nested_ops->free_context(new_ctx);

	if (reset)
		kvm_x86_ops.nested_ops->reset_context(ctx);

	return ctx;
}

void kvm_nested_context_clear(struct kvm_vcpu *vcpu, gpa_t gpa)
{
	struct kvm_nested_context_table *table;
	struct kvm_nested_context *ctx;

	table = vcpu->kvm->arch.nested_context_table;
	if (WARN_ON_ONCE(!table))
		return;

	spin_lock(&table->lock);
	ctx = __kvm_nested_context_find(table, gpa);
	if (ctx && ctx->vcpu) {
		/*
		 * Move to LRU list but keep it in the hash table for possible future
		 * reuse.
		 */
		list_add_tail(&ctx->lru_link, &table->lru_list);
		ctx->vcpu = NULL;
	}
	spin_unlock(&table->lock);
}
