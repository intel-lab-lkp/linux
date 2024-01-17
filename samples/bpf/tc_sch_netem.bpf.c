#include "vmlinux.h"
#include "bpf_experimental.h"
#include <bpf/bpf_helpers.h>

#define NETEM_DIST_SCALE	8192

#define NS_PER_SEC		1000000000

int q_loss_model = CLG_GILB_ELL;
unsigned int q_limit = 1000;
signed long q_latency = 0;
signed long q_jitter = 0;
unsigned int q_loss = 1;
unsigned int q_qlen = 0;

struct crndstate q_loss_cor = {.last = 0, .rho = 0,};
struct crndstate q_delay_cor = {.last = 0, .rho = 0,};

struct skb_node {
	u64 tstamp;
	struct sk_buff __kptr *skb;
	struct bpf_rb_node node;
};

struct clg_state {
	u64 state;
	u32 a1;
	u32 a2;
	u32 a3;
	u32 a4;
	u32 a5;
};

static bool skbn_tstamp_less(struct bpf_rb_node *a, const struct bpf_rb_node *b)
{
	struct skb_node *skb_a;
	struct skb_node *skb_b;

	skb_a = container_of(a, struct skb_node, node);
	skb_b = container_of(b, struct skb_node, node);

	return skb_a->tstamp < skb_b->tstamp;
}
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__type(key, __u32);
	__type(value, struct clg_state);
	__uint(max_entries, 1);
} g_clg_state SEC(".maps");

#define private(name) SEC(".data." #name) __hidden __attribute__((aligned(8)))

private(A) struct bpf_spin_lock t_root_lock;
private(A) struct bpf_rb_root t_root __contains(skb_node, node);

struct sk_buff *bpf_skb_acquire(struct sk_buff *p) __ksym;
void bpf_skb_release(struct sk_buff *p) __ksym;
u32 bpf_skb_get_hash(struct sk_buff *p) __ksym;
void bpf_qdisc_set_skb_dequeue(struct sk_buff *p) __ksym;

static __always_inline u32 get_crandom(struct crndstate *state)
{
	u64 value, rho;
	unsigned long answer;

	if (!state || state->rho == 0)	/* no correlation */
		return bpf_get_prandom_u32();

	value = bpf_get_prandom_u32();
	rho = (u64)state->rho + 1;
	answer = (value * ((1ull<<32) - rho) + state->last * rho) >> 32;
	state->last = answer;
	return answer;
}

static __always_inline s64 tabledist(s64 mu, s32 sigma, struct crndstate *state)
{
	s64 x;
	long t;
	u32 rnd;

	if (sigma == 0)
		return mu;

	rnd = get_crandom(state);

	/* default uniform distribution */
	return ((rnd % (2 * (u32)sigma)) + mu) - sigma;
}

static __always_inline bool loss_gilb_ell(void)
{
	struct clg_state *clg;
	u32 r1, r2, key = 0;
	bool ret = false;

 	clg = bpf_map_lookup_elem(&g_clg_state, &key);
	if (!clg)
		return false;

	r1 = bpf_get_prandom_u32();
	r2 = bpf_get_prandom_u32();

	switch (clg->state) {
	case GOOD_STATE:
		if (r1 < clg->a1)
			__sync_val_compare_and_swap(&clg->state,
						    GOOD_STATE, BAD_STATE);
		if (r2 < clg->a4)
			ret = true;
		break;
	case BAD_STATE:
		if (r1 < clg->a2)
			__sync_val_compare_and_swap(&clg->state,
						    BAD_STATE, GOOD_STATE);
		if (r2 > clg->a3)
			ret = true;
	}

	return ret;
}

static __always_inline bool loss_event(void)
{
	switch (q_loss_model) {
	case CLG_RANDOM:
		return q_loss && q_loss >= get_crandom(&q_loss_cor);
	case CLG_GILB_ELL:
		return loss_gilb_ell();
	}

	return false;
}

static __always_inline void tfifo_enqueue(struct skb_node *skbn)
{
	bpf_spin_lock(&t_root_lock);
	bpf_rbtree_add(&t_root, &skbn->node, skbn_tstamp_less);
	bpf_spin_unlock(&t_root_lock);
}

SEC("qdisc/enqueue")
int enqueue_prog(struct bpf_qdisc_ctx *ctx)
{
	struct sk_buff *old, *skb = ctx->skb;
	struct skb_node *skbn;
	int count = 1;
	s64 delay = 0;
	u64 now;

	if (loss_event())
		--count;

	if (count == 0)
		return SCH_BPF_BYPASS;

	q_qlen++;
	if (q_qlen > q_limit)
		return SCH_BPF_DROP;

	skb = bpf_skb_acquire(ctx->skb);
	skbn = bpf_obj_new(typeof(*skbn));
	if (!skbn) {
		bpf_skb_release(skb);
		return SCH_BPF_DROP;
	}

	delay = tabledist(q_latency, q_jitter, &q_delay_cor);

	now = bpf_ktime_get_ns();

	skbn->tstamp = now + delay;
	old = bpf_kptr_xchg(&skbn->skb, skb);
	if (old)
		bpf_skb_release(old);

	tfifo_enqueue(skbn);
	return SCH_BPF_QUEUED;
}


SEC("qdisc/dequeue")
int dequeue_prog(struct bpf_qdisc_ctx *ctx)
{
	struct bpf_rb_node *node = NULL;
	struct sk_buff *skb = NULL;
	struct skb_node *skbn;
	u64 now;

	now = bpf_ktime_get_ns();

	bpf_spin_lock(&t_root_lock);
	node = bpf_rbtree_first(&t_root);
	if (!node) {
		bpf_spin_unlock(&t_root_lock);
		return SCH_BPF_DROP;
	}

	skbn = container_of(node, struct skb_node, node);
	if (skbn->tstamp <= now) {
		node = bpf_rbtree_remove(&t_root, &skbn->node);
		bpf_spin_unlock(&t_root_lock);

		if (!node)
			return SCH_BPF_DROP;

		skbn = container_of(node, struct skb_node, node);
		skb = bpf_kptr_xchg(&skbn->skb, skb);
		if (!skb) {
			bpf_obj_drop(skbn);
			return SCH_BPF_DROP;
		}

		bpf_qdisc_set_skb_dequeue(skb);
		bpf_obj_drop(skbn);

		q_qlen--;
		return SCH_BPF_DEQUEUED;
	}

	ctx->expire = skbn->tstamp;
	bpf_spin_unlock(&t_root_lock);
	return SCH_BPF_THROTTLE;
}

static int reset_queue(u32 index, void *ctx)
{
	struct bpf_rb_node *node = NULL;
	struct skb_node *skbn;

	bpf_spin_lock(&t_root_lock);
	node = bpf_rbtree_first(&t_root);
	if (!node) {
		bpf_spin_unlock(&t_root_lock);
		return 1;
	}

	skbn = container_of(node, struct skb_node, node);
	node = bpf_rbtree_remove(&t_root, &skbn->node);
	bpf_spin_unlock(&t_root_lock);

	if (!node)
		return 1;

	skbn = container_of(node, struct skb_node, node);
	bpf_obj_drop(skbn);
	return 0;
}

SEC("qdisc/reset")
void reset_prog(struct bpf_qdisc_ctx *ctx)
{
	bpf_loop(q_limit, reset_queue, NULL, 0);
}

char _license[] SEC("license") = "GPL";
