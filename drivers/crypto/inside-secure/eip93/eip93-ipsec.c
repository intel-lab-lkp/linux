// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026
 *
 * Jihong Min <hurryman2212@gmail.com>
 */

#include <crypto/aes.h>
#include <crypto/eip93-ipsec.h>
#include <crypto/hash.h>
#include <crypto/hmac.h>
#include <crypto/sha1.h>
#include <crypto/sha2.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/ip.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netlink.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/refcount.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/udp.h>
#include <net/esp.h>
#include <net/xfrm.h>
#include <uapi/linux/pfkeyv2.h>

#include "eip93-main.h"
#include "eip93-regs.h"
#include "eip93-common.h"

#define EIP93_IPSEC_PAD_ALIGN 2
#define EIP93_IPSEC_IDR_MIN 0
#define EIP93_IPSEC_IDR_MAX (EIP93_RING_NUM - 1)
#define EIP93_IPSEC_DIGEST_WORD_BITS (BITS_PER_BYTE * sizeof(u32))
#define EIP93_IPSEC_DIGEST_WORDS(bits) ((bits) / EIP93_IPSEC_DIGEST_WORD_BITS)
#define EIP93_IPSEC_HMAC_STATE_SIZE SHA256_DIGEST_SIZE
#define EIP93_IPSEC_PRNG_BUF_SIZE 4080
#define EIP93_IPSEC_PRNG_RESET_MODE 1
#define EIP93_IPSEC_PRNG_POLL_US 10000
#define EIP93_IPSEC_PRNG_POLL_STEP_US 10

struct eip93_ipsec {
	struct eip93_device *eip93;
	struct list_head node;
	struct list_head sa_list;
	struct work_struct fault_work;
	spinlock_t lock; /* protects dead/refcount admission */
	refcount_t refcnt;
	struct completion done;
	enum eip93_ipsec_event fault_event;
	u32 algo_flags;
	bool dead;
};

struct eip93_ipsec_sa {
	struct eip93_ipsec *ipsec;
	struct sa_record *sa_record;
	dma_addr_t sa_record_base;
	struct list_head node;
	struct list_head requests;
	spinlock_t lock; /* protects dead/refcount admission */
	refcount_t refcnt;
	struct completion done;
	u32 flags;
	u16 family;
	u8 authsize;
	u8 blocksize;
	u8 ivsize;
	u8 encap_type;
	bool esn;
	bool dead;
	bool aborting;
};

struct eip93_ipsec_request {
	struct eip93_ipsec_sa *sa;
	struct sk_buff *skb;
	struct list_head node;
	refcount_t refcnt;
	eip93_ipsec_complete_t complete;
	void *data;
	dma_addr_t dma;
	unsigned int dma_len;
	enum dma_data_direction dma_dir;
	int idr;
};

static DEFINE_MUTEX(eip93_ipsec_devices_lock);
static LIST_HEAD(eip93_ipsec_devices);
static BLOCKING_NOTIFIER_HEAD(eip93_ipsec_notifier);

static bool eip93_ipsec_get_ref(struct eip93_ipsec *ipsec)
{
	bool ret = false;

	spin_lock_bh(&ipsec->lock);
	if (!ipsec->dead)
		ret = refcount_inc_not_zero(&ipsec->refcnt);
	spin_unlock_bh(&ipsec->lock);

	return ret;
}

void eip93_ipsec_put(struct eip93_ipsec *ipsec)
{
	if (ipsec && refcount_dec_and_test(&ipsec->refcnt))
		complete(&ipsec->done);
}
EXPORT_SYMBOL_GPL(eip93_ipsec_put);

static bool eip93_ipsec_same_subsystem(struct device *consumer,
				       struct eip93_ipsec *ipsec)
{
	struct device_node *consumer_parent;
	struct device_node *eip93_parent;
	struct device_node *consumer_np;
	struct device_node *eip93_np;
	bool match;

	consumer_np = dev_of_node(consumer);
	eip93_np = dev_of_node(ipsec->eip93->dev);
	if (!consumer_np || !eip93_np)
		return false;

	consumer_parent = of_get_parent(consumer_np);
	eip93_parent = of_get_parent(eip93_np);
	match = consumer_parent && consumer_parent == eip93_parent;
	of_node_put(consumer_parent);
	of_node_put(eip93_parent);

	return match;
}

static bool eip93_ipsec_hw_available(u32 flags)
{
	if (!(flags & EIP93_PE_OPTION_AES))
		return false;

	if (!(flags & (EIP93_PE_OPTION_AES_KEY128 | EIP93_PE_OPTION_AES_KEY192 |
		       EIP93_PE_OPTION_AES_KEY256)))
		return false;

	return flags & (EIP93_PE_OPTION_SHA_1 | EIP93_PE_OPTION_SHA_256);
}

static bool eip93_ipsec_mark_dead(struct eip93_ipsec *ipsec)
{
	bool marked = false;

	spin_lock_bh(&ipsec->lock);
	if (!ipsec->dead) {
		ipsec->dead = true;
		marked = true;
	}
	spin_unlock_bh(&ipsec->lock);

	return marked;
}

static bool eip93_ipsec_mark_dead_async(struct eip93_ipsec *ipsec,
					enum eip93_ipsec_event event)
{
	bool marked = false;

	spin_lock_bh(&ipsec->lock);
	if (!ipsec->dead && refcount_inc_not_zero(&ipsec->refcnt)) {
		ipsec->dead = true;
		ipsec->fault_event = event;
		marked = true;
	}
	spin_unlock_bh(&ipsec->lock);

	if (marked)
		schedule_work(&ipsec->fault_work);

	return marked;
}

static bool eip93_ipsec_live_hw_available(struct eip93_ipsec *ipsec)
{
	u32 flags = readl(ipsec->eip93->base + EIP93_REG_PE_OPTION_1);

	spin_lock_bh(&ipsec->lock);
	ipsec->algo_flags = flags;
	spin_unlock_bh(&ipsec->lock);

	return eip93_ipsec_hw_available(flags);
}

struct eip93_ipsec *eip93_ipsec_get(struct device *consumer)
{
	struct eip93_ipsec *ipsec;
	int err = -ENODEV;

	if (!consumer)
		return ERR_PTR(-EINVAL);

	mutex_lock(&eip93_ipsec_devices_lock);
	list_for_each_entry(ipsec, &eip93_ipsec_devices, node) {
		if (!eip93_ipsec_same_subsystem(consumer, ipsec))
			continue;

		if (!eip93_ipsec_live_hw_available(ipsec)) {
			enum eip93_ipsec_event event;

			event = EIP93_IPSEC_EVENT_CAPABILITY_LOSS;
			eip93_ipsec_mark_dead_async(ipsec, event);
			err = -EOPNOTSUPP;
			continue;
		}

		if (!eip93_ipsec_get_ref(ipsec)) {
			err = -ENODEV;
			continue;
		}

		mutex_unlock(&eip93_ipsec_devices_lock);
		return ipsec;
	}
	mutex_unlock(&eip93_ipsec_devices_lock);

	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(eip93_ipsec_get);

bool eip93_ipsec_available(struct eip93_ipsec *ipsec)
{
	bool available;

	if (!ipsec)
		return false;

	spin_lock_bh(&ipsec->lock);
	available = !ipsec->dead;
	spin_unlock_bh(&ipsec->lock);
	if (!available)
		return false;

	available = eip93_ipsec_live_hw_available(ipsec);
	if (!available)
		eip93_ipsec_mark_dead_async(ipsec,
					    EIP93_IPSEC_EVENT_CAPABILITY_LOSS);

	return available;
}
EXPORT_SYMBOL_GPL(eip93_ipsec_available);

u32 eip93_ipsec_features(struct eip93_ipsec *ipsec)
{
	if (!eip93_ipsec_available(ipsec))
		return 0;

	return EIP93_IPSEC_FEATURE_ESP;
}
EXPORT_SYMBOL_GPL(eip93_ipsec_features);

int eip93_ipsec_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&eip93_ipsec_notifier, nb);
}
EXPORT_SYMBOL_GPL(eip93_ipsec_register_notifier);

void eip93_ipsec_unregister_notifier(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&eip93_ipsec_notifier, nb);
}
EXPORT_SYMBOL_GPL(eip93_ipsec_unregister_notifier);

static bool eip93_ipsec_sa_get(struct eip93_ipsec_sa *sa)
{
	bool ret = false;

	spin_lock_bh(&sa->ipsec->lock);
	spin_lock(&sa->lock);
	if (!sa->ipsec->dead && !sa->dead)
		ret = refcount_inc_not_zero(&sa->refcnt);
	spin_unlock(&sa->lock);
	spin_unlock_bh(&sa->ipsec->lock);

	return ret;
}

static void eip93_ipsec_sa_put(struct eip93_ipsec_sa *sa)
{
	if (refcount_dec_and_test(&sa->refcnt))
		complete(&sa->done);
}

static bool eip93_ipsec_request_get(struct eip93_ipsec_request *req)
{
	return refcount_inc_not_zero(&req->refcnt);
}

static void eip93_ipsec_request_put(struct eip93_ipsec_request *req)
{
	if (refcount_dec_and_test(&req->refcnt))
		kfree(req);
}

static int eip93_ipsec_parse_flags(struct xfrm_state *x, u32 *flags)
{
	switch (x->props.ealgo) {
	case SADB_X_EALG_AESCBC:
		*flags |= EIP93_ALG_AES | EIP93_MODE_CBC;
		break;
	default:
		return -EOPNOTSUPP;
	}

	switch (x->props.aalgo) {
	case SADB_AALG_SHA1HMAC:
		*flags |= EIP93_HASH_HMAC | EIP93_HASH_SHA1;
		break;
	case SADB_X_AALG_SHA2_256HMAC:
		*flags |= EIP93_HASH_HMAC | EIP93_HASH_SHA256;
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (x->xso.dir == XFRM_DEV_OFFLOAD_IN)
		*flags |= EIP93_DECRYPT;
	else
		*flags |= EIP93_ENCRYPT;

	return 0;
}

static unsigned int eip93_ipsec_auth_digest_size(struct xfrm_state *x)
{
	switch (x->props.aalgo) {
	case SADB_AALG_SHA1HMAC:
		return SHA1_DIGEST_SIZE;
	case SADB_X_AALG_SHA2_256HMAC:
		return SHA256_DIGEST_SIZE;
	default:
		return 0;
	}
}

static int eip93_ipsec_hmac_setkey(u32 flags, const u8 *key,
				   unsigned int keylen, u8 *dest_ipad,
				   u8 *dest_opad)
{
	u8 *ipad, *opad;
	struct crypto_shash *tfm;
	const char *alg_name;
	unsigned int blocksize;
	unsigned int digestsize;
	unsigned int statesize;
	unsigned int alloc_size;
	unsigned int i;
	int err;

	switch (flags & EIP93_HASH_MASK) {
	case EIP93_HASH_SHA1:
		alg_name = "sha1";
		digestsize = SHA1_DIGEST_SIZE;
		break;
	case EIP93_HASH_SHA256:
		alg_name = "sha256";
		digestsize = SHA256_DIGEST_SIZE;
		break;
	default:
		return -EINVAL;
	}

	tfm = crypto_alloc_shash(alg_name, 0, CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	blocksize = crypto_shash_blocksize(tfm);
	statesize = crypto_shash_statesize(tfm);
	if (statesize < EIP93_IPSEC_HMAC_STATE_SIZE) {
		err = -EINVAL;
		goto free_tfm;
	}

	alloc_size = 2 * (blocksize + statesize);
	ipad = kzalloc(alloc_size, GFP_KERNEL);
	if (!ipad) {
		err = -ENOMEM;
		goto free_tfm;
	}
	opad = ipad + blocksize + statesize;

	if (keylen > blocksize) {
		SHASH_DESC_ON_STACK(desc, tfm);

		desc->tfm = tfm;
		err = crypto_shash_digest(desc, key, keylen, ipad);
		shash_desc_zero(desc);
		if (err)
			goto free_pad;

		keylen = digestsize;
	} else {
		memcpy(ipad, key, keylen);
	}

	memcpy(opad, ipad, blocksize);
	for (i = 0; i < blocksize; i++) {
		ipad[i] ^= HMAC_IPAD_VALUE;
		opad[i] ^= HMAC_OPAD_VALUE;
	}

	{
		SHASH_DESC_ON_STACK(desc, tfm);

		desc->tfm = tfm;
		err = crypto_shash_init(desc) ?:
		      crypto_shash_update(desc, ipad, blocksize) ?:
		      crypto_shash_export(desc, ipad) ?:
		      crypto_shash_init(desc) ?:
		      crypto_shash_update(desc, opad, blocksize) ?:
		      crypto_shash_export(desc, opad);
		shash_desc_zero(desc);
	}
	if (err)
		goto free_pad;

	/*
	 * EIP93 ESP protocol mode consumes the raw exported HMAC ipad/opad
	 * state. The crypto API AEAD helper byteswaps this state for its basic
	 * authenc path, but packet ESP mode matches mtk-eip93 with native
	 * exported bytes in the SA record.
	 */
	memcpy(dest_ipad, ipad, EIP93_IPSEC_HMAC_STATE_SIZE);
	memcpy(dest_opad, opad, EIP93_IPSEC_HMAC_STATE_SIZE);

free_pad:
	kfree_sensitive(ipad);
free_tfm:
	crypto_free_shash(tfm);
	return err;
}

static int eip93_ipsec_validate_algo(struct xfrm_state *x,
				     struct netlink_ext_ack *extack)
{
	unsigned int authsize;
	unsigned int keylen;

	if (x->aead) {
		NL_SET_ERR_MSG_MOD(extack, "AEAD SAs are unsupported");
		return -EOPNOTSUPP;
	}

	if (!x->ealg || !x->aalg) {
		NL_SET_ERR_MSG_MOD(extack, "encryption/auth required");
		return -EOPNOTSUPP;
	}

	if (x->props.ealgo != SADB_X_EALG_AESCBC) {
		NL_SET_ERR_MSG_MOD(extack, "only AES-CBC is supported");
		return -EOPNOTSUPP;
	}

	keylen = x->ealg->alg_key_len / BITS_PER_BYTE;
	if (keylen != AES_KEYSIZE_128 && keylen != AES_KEYSIZE_192 &&
	    keylen != AES_KEYSIZE_256) {
		NL_SET_ERR_MSG_MOD(extack, "unsupported AES-CBC key length");
		return -EOPNOTSUPP;
	}

	authsize = eip93_ipsec_auth_digest_size(x);
	if (!authsize) {
		NL_SET_ERR_MSG_MOD(extack, "only SHA1/SHA256 HMAC");
		return -EOPNOTSUPP;
	}

	if (x->aalg->alg_trunc_len % EIP93_IPSEC_DIGEST_WORD_BITS ||
	    x->aalg->alg_trunc_len < EIP93_IPSEC_DIGEST_WORD_BITS ||
	    x->aalg->alg_trunc_len > authsize * BITS_PER_BYTE) {
		NL_SET_ERR_MSG_MOD(extack, "bad auth truncation length");
		return -EOPNOTSUPP;
	}

	return 0;
}

static int eip93_ipsec_validate_state(struct xfrm_state *x,
				      struct netlink_ext_ack *extack)
{
	switch (x->xso.dir) {
	case XFRM_DEV_OFFLOAD_OUT:
	case XFRM_DEV_OFFLOAD_IN:
		break;
	default:
		NL_SET_ERR_MSG_MOD(extack, "only in/out SAs are supported");
		return -EOPNOTSUPP;
	}

	if (x->xso.type != XFRM_DEV_OFFLOAD_CRYPTO) {
		NL_SET_ERR_MSG_MOD(extack, "only crypto offload is supported");
		return -EOPNOTSUPP;
	}

	if (x->id.proto != IPPROTO_ESP) {
		NL_SET_ERR_MSG_MOD(extack,
				   "EIP93 packet backend supports ESP only");
		return -EOPNOTSUPP;
	}

	switch (x->props.family) {
	case AF_INET:
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case AF_INET6:
		break;
#endif
	default:
		NL_SET_ERR_MSG_MOD(extack, "only IPv4/IPv6 is supported");
		return -EOPNOTSUPP;
	}

	if (x->outer_mode.family != x->props.family) {
		NL_SET_ERR_MSG_MOD(extack, "only same-family ESP is supported");
		return -EOPNOTSUPP;
	}

	switch (x->props.mode) {
	case XFRM_MODE_TUNNEL:
	case XFRM_MODE_TRANSPORT:
		break;
	default:
		NL_SET_ERR_MSG_MOD(extack, "only tunnel/transport");
		return -EOPNOTSUPP;
	}

	if (x->outer_mode.encap != x->props.mode) {
		NL_SET_ERR_MSG_MOD(extack,
				   "outer ESP mode does not match state mode");
		return -EOPNOTSUPP;
	}

	if (x->encap) {
		NL_SET_ERR_MSG_MOD(extack,
				   "NAT-T is unsupported by EIP93 packet ESP");
		return -EOPNOTSUPP;
	}

	if (x->tfcpad) {
		NL_SET_ERR_MSG_MOD(extack, "TFC padding is unsupported");
		return -EOPNOTSUPP;
	}

	return eip93_ipsec_validate_algo(x, extack);
}

static int eip93_ipsec_validate_hw(struct xfrm_state *x, u32 flags,
				   struct netlink_ext_ack *extack)
{
	unsigned int keylen = x->ealg->alg_key_len / BITS_PER_BYTE;
	u32 required;

	if (!(flags & EIP93_PE_OPTION_AES)) {
		NL_SET_ERR_MSG_MOD(extack, "EIP93 AES engine is unavailable");
		return -EOPNOTSUPP;
	}

	switch (keylen) {
	case AES_KEYSIZE_128:
		required = EIP93_PE_OPTION_AES_KEY128;
		break;
	case AES_KEYSIZE_192:
		required = EIP93_PE_OPTION_AES_KEY192;
		break;
	case AES_KEYSIZE_256:
		required = EIP93_PE_OPTION_AES_KEY256;
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (!(flags & required)) {
		NL_SET_ERR_MSG_MOD(extack, "unsupported AES key length");
		return -EOPNOTSUPP;
	}

	switch (x->props.aalgo) {
	case SADB_AALG_SHA1HMAC:
		required = EIP93_PE_OPTION_SHA_1;
		break;
	case SADB_X_AALG_SHA2_256HMAC:
		required = EIP93_PE_OPTION_SHA_256;
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (!(flags & required)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "EIP93 does not support this HMAC hash");
		return -EOPNOTSUPP;
	}

	return 0;
}

static void eip93_ipsec_init_sa_record(struct eip93_ipsec_sa *sa,
				       struct xfrm_state *x)
{
	struct sa_record *record = sa->sa_record;
	unsigned int auth_words;
	unsigned int enckeylen;

	enckeylen = x->ealg->alg_key_len / BITS_PER_BYTE;
	auth_words = EIP93_IPSEC_DIGEST_WORDS(x->aalg->alg_trunc_len);

	eip93_set_sa_record(record, enckeylen, sa->flags);

	record->sa_cmd0_word &=
		~(EIP93_SA_CMD_OPGROUP | EIP93_SA_CMD_OPCODE |
		  EIP93_SA_CMD_DIGEST_LENGTH | EIP93_SA_CMD_PAD_TYPE |
		  EIP93_SA_CMD_IV_SOURCE | EIP93_SA_CMD_SAVE_IV);
	record->sa_cmd0_word |=
		EIP93_SA_CMD_OP_PROTOCOL | EIP93_SA_CMD_HDR_PROC |
		EIP93_SA_CMD_PAD_IPSEC | EIP93_SA_CMD_SCPAD |
		FIELD_PREP(EIP93_SA_CMD_OPCODE,
			   EIP93_SA_CMD_OPCODE_PROTOCOL_OUT_ESP) |
		FIELD_PREP(EIP93_SA_CMD_DIGEST_LENGTH, auth_words);

	/*
	 * ESP packet mode authenticates from the ESP header when the hash
	 * crypt offset is zero. This is intentionally different from the AEAD
	 * authenc path, whose AAD starts after the ESP header.
	 */
	record->sa_cmd1_word &=
		~(EIP93_SA_CMD_HASH_CRYPT_OFFSET | EIP93_SA_CMD_BYTE_OFFSET |
		  EIP93_SA_CMD_COPY_PAD | EIP93_SA_CMD_COPY_HEADER |
		  EIP93_SA_CMD_COPY_DIGEST | EIP93_SA_CMD_COPY_PAYLOAD |
		  EIP93_SA_CMD_EN_SEQNUM_CHK);
	record->sa_cmd1_word |= EIP93_SA_CMD_HMAC | EIP93_SA_CMD_EN_SEQNUM_CHK;

	if (x->xso.dir == XFRM_DEV_OFFLOAD_IN) {
		record->sa_cmd0_word |= EIP93_SA_CMD_DIRECTION_IN |
					EIP93_SA_CMD_IV_FROM_INPUT;
		/*
		 * Inbound ESP decapsulation keeps the outer header for XFRM and
		 * lets hardware remove ESP pad/trailer/ICV bytes.
		 */
		record->sa_cmd1_word |= EIP93_SA_CMD_COPY_HEADER;
	} else {
		record->sa_cmd0_word |= EIP93_SA_CMD_IV_FROM_PRNG;
		record->sa_cmd1_word |= EIP93_SA_CMD_COPY_DIGEST;
	}

	record->sa_spi = ntohl(x->id.spi);
	if (sa->esn && x->replay_esn) {
		if (x->xso.dir == XFRM_DEV_OFFLOAD_IN)
			record->sa_seqnum[1] = x->replay_esn->seq_hi;
		else
			record->sa_seqnum[1] = x->replay_esn->oseq_hi;
	} else {
		record->sa_seqnum[1] = 0;
	}
	record->sa_seqnum[0] = 0;
	record->sa_seqmum_mask[0] = 0xffffffff;
	record->sa_seqmum_mask[1] = sa->esn ? 0xffffffff : 0;
}

static int eip93_ipsec_poll_result(struct eip93_device *eip93,
				   struct eip93_descriptor **rdesc)
{
	struct eip93_descriptor *desc;
	unsigned int i;
	u32 pe_ctrl_stat;
	u32 pe_length;

	for (i = 0; i < EIP93_IPSEC_PRNG_POLL_US;
	     i += EIP93_IPSEC_PRNG_POLL_STEP_US) {
		if (readl(eip93->base + EIP93_REG_PE_RD_COUNT) &
		    EIP93_PE_RD_COUNT)
			break;
		udelay(EIP93_IPSEC_PRNG_POLL_STEP_US);
	}
	if (i >= EIP93_IPSEC_PRNG_POLL_US)
		return -ETIMEDOUT;

	scoped_guard(spinlock_irqsave, &eip93->ring->read_lock)
		desc = eip93_get_descriptor(eip93);
	if (IS_ERR(desc))
		return PTR_ERR(desc);
	*rdesc = desc;

	for (i = 0; i < EIP93_IPSEC_PRNG_POLL_US;
	     i += EIP93_IPSEC_PRNG_POLL_STEP_US) {
		pe_ctrl_stat = READ_ONCE((*rdesc)->pe_ctrl_stat_word);
		pe_length = READ_ONCE((*rdesc)->pe_length_word);
		if (FIELD_GET(EIP93_PE_CTRL_PE_READY_DES_TRING_OWN,
			      pe_ctrl_stat) == EIP93_PE_CTRL_PE_READY &&
		    FIELD_GET(EIP93_PE_LENGTH_HOST_PE_READY, pe_length) ==
			    EIP93_PE_LENGTH_PE_READY)
			break;
		udelay(EIP93_IPSEC_PRNG_POLL_STEP_US);
	}

	writel(1, eip93->base + EIP93_REG_PE_RD_COUNT);
	writel(EIP93_INT_RDR_THRESH, eip93->base + EIP93_REG_INT_CLR);

	if (i >= EIP93_IPSEC_PRNG_POLL_US)
		return -ETIMEDOUT;

	return 0;
}

static int eip93_ipsec_init_prng(struct eip93_device *eip93)
{
	static const u32 prng_dt[4] = {};
	static const u32 prng_key[4] = {
		0xe0fc631d, 0xcbb9fb9a, 0x869285cb, 0xcbb9fb9a
	};
	static const u32 prng_seed[4] = {
		0x758bac03, 0xf20ab39e, 0xa569f104, 0x95dfaea6
	};
	struct eip93_descriptor cdesc = {};
	struct eip93_descriptor *rdesc;
	struct sa_record *record;
	dma_addr_t record_dma;
	dma_addr_t buf_dma;
	void *buf;
	int err;

	record = dma_alloc_coherent(eip93->dev, sizeof(*record), &record_dma,
				    GFP_KERNEL);
	if (!record)
		return -ENOMEM;

	buf = dma_alloc_coherent(eip93->dev, EIP93_IPSEC_PRNG_BUF_SIZE,
				 &buf_dma, GFP_KERNEL);
	if (!buf) {
		err = -ENOMEM;
		goto free_record;
	}

	memset(record, 0, sizeof(*record));
	record->sa_cmd0_word =
		EIP93_SA_CMD_OP_BASIC |
		FIELD_PREP(EIP93_SA_CMD_OPCODE,
			   EIP93_SA_CMD_OPCODE_BASIC_OUT_PRNG) |
		EIP93_SA_CMD_CIPHER_AES | EIP93_SA_CMD_HASH_SHA1;
	record->sa_cmd1_word = EIP93_SA_CMD_AES_KEY_128BIT;
	memcpy(record->sa_key, prng_key, sizeof(prng_key));
	memcpy(record->sa_i_digest, prng_seed, sizeof(prng_seed));
	memcpy(record->sa_o_digest, prng_dt, sizeof(prng_dt));

	cdesc.pe_ctrl_stat_word =
		FIELD_PREP(EIP93_PE_CTRL_PE_READY_DES_TRING_OWN,
			   EIP93_PE_CTRL_HOST_READY) |
		FIELD_PREP(EIP93_PE_CTRL_PE_PRNG_MODE,
			   EIP93_IPSEC_PRNG_RESET_MODE);
	cdesc.dst_addr = (u32 __force)buf_dma;
	cdesc.sa_addr = record_dma;
	cdesc.user_id = FIELD_PREP(EIP93_PE_USER_ID_DESC_FLAGS,
				   EIP93_DESC_PRNG | EIP93_DESC_LAST);
	cdesc.pe_length_word =
		FIELD_PREP(EIP93_PE_LENGTH_HOST_PE_READY,
			   EIP93_PE_LENGTH_HOST_READY) |
		FIELD_PREP(EIP93_PE_LENGTH_LENGTH,
			   EIP93_IPSEC_PRNG_BUF_SIZE);

	/*
	 * Outbound ESP SAs use IV_FROM_PRNG. Initialize the EIP93 PRNG before
	 * exposing the packet backend, otherwise the first ESP encrypt can
	 * fail or emit unusable IV material.
	 */
	scoped_guard(spinlock_irqsave, &eip93->ring->write_lock)
		err = eip93_put_descriptor(eip93, &cdesc);
	if (err)
		goto free_buf;

	writel(1, eip93->base + EIP93_REG_PE_CD_COUNT);
	err = eip93_ipsec_poll_result(eip93, &rdesc);
	if (err)
		goto free_buf;

	err = rdesc->pe_ctrl_stat_word &
	      (EIP93_PE_CTRL_PE_EXT_ERR_CODE | EIP93_PE_CTRL_PE_EXT_ERR |
	       EIP93_PE_CTRL_PE_SEQNUM_ERR | EIP93_PE_CTRL_PE_PAD_ERR |
	       EIP93_PE_CTRL_PE_AUTH_ERR);
	err = eip93_parse_ctrl_stat_err(eip93, err);
	if (err)
		dev_err(eip93->dev, "IPsec PRNG init failed: %d\n", err);

free_buf:
	dma_free_coherent(eip93->dev, EIP93_IPSEC_PRNG_BUF_SIZE, buf, buf_dma);
free_record:
	dma_free_coherent(eip93->dev, sizeof(*record), record, record_dma);

	return err;
}

static int eip93_ipsec_submit(struct eip93_ipsec_request *req,
			      struct eip93_descriptor *cdesc)
{
	struct eip93_device *eip93 = req->sa->ipsec->eip93;
	struct eip93_ipsec_sa *sa = req->sa;
	struct eip93_ipsec *ipsec = sa->ipsec;
	int err;

	spin_lock_bh(&ipsec->lock);
	if (ipsec->dead) {
		err = -EOPNOTSUPP;
		goto unlock_ipsec;
	}

	scoped_guard(spinlock_bh, &eip93->ring->idr_lock) req->idr =
		idr_alloc(&eip93->ring->crypto_async_idr, req,
			  EIP93_IPSEC_IDR_MIN, EIP93_IPSEC_IDR_MAX, GFP_ATOMIC);
	if (req->idr < 0) {
		err = req->idr == -ENOSPC ? -EBUSY : req->idr;
		goto unlock_ipsec;
	}

	spin_lock(&sa->lock);
	if (sa->dead) {
		spin_unlock(&sa->lock);
		err = -EOPNOTSUPP;
		goto remove_idr;
	}
	list_add_tail(&req->node, &sa->requests);
	spin_unlock(&sa->lock);

	cdesc->user_id =
		FIELD_PREP(EIP93_PE_USER_ID_CRYPTO_IDR, (u16)req->idr) |
		FIELD_PREP(EIP93_PE_USER_ID_DESC_FLAGS,
			   EIP93_DESC_IPSEC | EIP93_DESC_LAST);

	scoped_guard(spinlock_irqsave, &eip93->ring->write_lock)
		err = eip93_put_descriptor(eip93, cdesc);
	if (err)
		goto unlink_request;

	writel(1, eip93->base + EIP93_REG_PE_CD_COUNT);
	spin_unlock_bh(&ipsec->lock);

	return -EINPROGRESS;

unlink_request:
	spin_lock(&sa->lock);
	list_del_init(&req->node);
	spin_unlock(&sa->lock);
remove_idr:
	scoped_guard(spinlock_bh, &eip93->ring->idr_lock)
		idr_remove(&eip93->ring->crypto_async_idr, req->idr);
	err = err == -ENOENT ? -EBUSY : err;
unlock_ipsec:
	spin_unlock_bh(&ipsec->lock);
	return err;
}

static void eip93_ipsec_unlink_request(struct eip93_ipsec_request *req)
{
	struct eip93_ipsec_sa *sa = req->sa;

	spin_lock_bh(&sa->lock);
	if (!list_empty(&req->node))
		list_del_init(&req->node);
	spin_unlock_bh(&sa->lock);
}

static void eip93_ipsec_complete_request(struct eip93_ipsec_request *req,
					 int err,
					 struct eip93_ipsec_result result)
{
	struct eip93_ipsec_sa *sa = req->sa;
	eip93_ipsec_complete_t complete = req->complete;
	void *data = req->data;

	dma_unmap_single(sa->ipsec->eip93->dev, req->dma, req->dma_len,
			 req->dma_dir);
	eip93_ipsec_unlink_request(req);
	eip93_ipsec_sa_put(sa);
	complete(data, err, result);
	eip93_ipsec_request_put(req);
}

static void eip93_ipsec_abort_sa(struct eip93_ipsec_sa *sa, int err)
{
	struct eip93_device *eip93 = sa->ipsec->eip93;
	struct eip93_ipsec_request *req;
	bool claimed;

	while (true) {
		spin_lock_bh(&sa->lock);
		if (list_empty(&sa->requests)) {
			spin_unlock_bh(&sa->lock);
			return;
		}

		req = list_first_entry(&sa->requests,
				       struct eip93_ipsec_request, node);
		if (!eip93_ipsec_request_get(req)) {
			list_del_init(&req->node);
			spin_unlock_bh(&sa->lock);
			continue;
		}
		list_del_init(&req->node);
		spin_unlock_bh(&sa->lock);

		claimed = false;
		scoped_guard(spinlock_bh, &eip93->ring->idr_lock) {
			if (idr_find(&eip93->ring->crypto_async_idr,
				     req->idr) == req) {
				idr_remove(&eip93->ring->crypto_async_idr,
					   req->idr);
				claimed = true;
			}
		}

		if (claimed) {
			struct eip93_ipsec_result result = {};

			eip93_ipsec_complete_request(req, err, result);
		}
		eip93_ipsec_request_put(req);
	}
}

static void eip93_ipsec_abort_requests(struct eip93_ipsec *ipsec, int err)
{
	struct eip93_ipsec_sa *sa;

	while (true) {
		bool found = false;

		spin_lock_bh(&ipsec->lock);
		list_for_each_entry(sa, &ipsec->sa_list, node) {
			spin_lock(&sa->lock);
			if (sa->aborting) {
				spin_unlock(&sa->lock);
				continue;
			}

			sa->aborting = true;
			found = refcount_inc_not_zero(&sa->refcnt);
			spin_unlock(&sa->lock);
			if (found)
				break;
		}
		spin_unlock_bh(&ipsec->lock);
		if (!found)
			return;

		eip93_ipsec_abort_sa(sa, err);
		eip93_ipsec_sa_put(sa);
	}
}

static void eip93_ipsec_fault_work(struct work_struct *work)
{
	struct eip93_ipsec *ipsec =
		container_of(work, struct eip93_ipsec, fault_work);
	enum eip93_ipsec_event event;

	spin_lock_bh(&ipsec->lock);
	event = ipsec->fault_event;
	spin_unlock_bh(&ipsec->lock);

	eip93_ipsec_abort_requests(ipsec, -EIO);
	blocking_notifier_call_chain(&eip93_ipsec_notifier, event, ipsec);
	eip93_ipsec_put(ipsec);
}

void eip93_ipsec_handle_result(struct eip93_ipsec_request *req, int err,
			       u32 pe_ctrl_stat, u32 pe_length)
{
	struct eip93_ipsec_result result = {};

	if (!req)
		return;

	if (err == -EIO || err == -EACCES)
		eip93_ipsec_mark_dead_async(req->sa->ipsec,
					    EIP93_IPSEC_EVENT_DMA_ERROR);

	if (!err) {
		result.packet_len = FIELD_GET(EIP93_PE_LENGTH_LENGTH, pe_length);
		result.nexthdr = FIELD_GET(EIP93_PE_CTRL_PE_PAD_VALUE,
					   pe_ctrl_stat);
	}

	eip93_ipsec_complete_request(req, err, result);
}

void eip93_ipsec_report_irq(struct eip93_device *eip93, u32 irq_status)
{
	struct eip93_ipsec *ipsec = eip93->ipsec;

	if (!ipsec)
		return;

	if (irq_status & EIP93_INT_HALT) {
		eip93_ipsec_mark_dead_async(ipsec, EIP93_IPSEC_EVENT_RESET);
		return;
	}

	if (irq_status & (EIP93_INT_INTERFACE_ERR | EIP93_INT_RPOC_ERR |
			  EIP93_INT_PE_RING_ERR))
		eip93_ipsec_mark_dead_async(ipsec, EIP93_IPSEC_EVENT_DMA_ERROR);
}

int eip93_ipsec_register(struct eip93_device *eip93)
{
	struct eip93_ipsec *ipsec;
	int err;

	ipsec = kzalloc(sizeof(*ipsec), GFP_KERNEL);
	if (!ipsec)
		return -ENOMEM;

	err = eip93_ipsec_init_prng(eip93);
	if (err) {
		kfree(ipsec);
		return err;
	}

	ipsec->eip93 = eip93;
	ipsec->algo_flags = readl(eip93->base + EIP93_REG_PE_OPTION_1);
	ipsec->fault_event = EIP93_IPSEC_EVENT_REMOVE;
	INIT_WORK(&ipsec->fault_work, eip93_ipsec_fault_work);
	spin_lock_init(&ipsec->lock);
	refcount_set(&ipsec->refcnt, 1);
	init_completion(&ipsec->done);
	INIT_LIST_HEAD(&ipsec->node);
	INIT_LIST_HEAD(&ipsec->sa_list);

	mutex_lock(&eip93_ipsec_devices_lock);
	eip93->ipsec = ipsec;
	list_add_tail(&ipsec->node, &eip93_ipsec_devices);
	mutex_unlock(&eip93_ipsec_devices_lock);

	return 0;
}

void eip93_ipsec_unregister(struct eip93_device *eip93)
{
	struct eip93_ipsec *ipsec = eip93->ipsec;
	bool notify_remove;

	if (!ipsec)
		return;

	mutex_lock(&eip93_ipsec_devices_lock);
	notify_remove = eip93_ipsec_mark_dead(ipsec);
	list_del_init(&ipsec->node);
	eip93->ipsec = NULL;
	mutex_unlock(&eip93_ipsec_devices_lock);

	eip93_ipsec_abort_requests(ipsec, -ENODEV);
	if (notify_remove)
		blocking_notifier_call_chain(&eip93_ipsec_notifier,
					     EIP93_IPSEC_EVENT_REMOVE, ipsec);

	eip93_ipsec_put(ipsec);
	wait_for_completion(&ipsec->done);
	cancel_work_sync(&ipsec->fault_work);
	kfree(ipsec);
}

int eip93_ipsec_state_add(struct eip93_ipsec *ipsec, struct xfrm_state *x,
			  struct netlink_ext_ack *extack,
			  struct eip93_ipsec_sa **sa)
{
	struct eip93_device *eip93;
	struct eip93_ipsec_sa *new_sa;
	unsigned int authkeylen;
	unsigned int enckeylen;
	int err;

	if (!ipsec || !eip93_ipsec_get_ref(ipsec)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "EIP93 packet backend is unavailable");
		return -EOPNOTSUPP;
	}

	err = eip93_ipsec_validate_state(x, extack);
	if (err)
		goto put_ipsec;

	err = eip93_ipsec_validate_hw(x, ipsec->algo_flags, extack);
	if (err)
		goto put_ipsec;

	eip93 = ipsec->eip93;
	new_sa = kzalloc(sizeof(*new_sa), GFP_KERNEL);
	if (!new_sa) {
		err = -ENOMEM;
		goto put_ipsec;
	}

	new_sa->ipsec = ipsec;
	new_sa->family = x->props.family;
	new_sa->ivsize = AES_BLOCK_SIZE;
	new_sa->authsize = x->aalg->alg_trunc_len / BITS_PER_BYTE;
	new_sa->blocksize = AES_BLOCK_SIZE;
	new_sa->encap_type = x->encap ? x->encap->encap_type : 0;
	new_sa->esn = x->props.flags & XFRM_STATE_ESN;
	INIT_LIST_HEAD(&new_sa->node);
	INIT_LIST_HEAD(&new_sa->requests);
	spin_lock_init(&new_sa->lock);
	refcount_set(&new_sa->refcnt, 1);
	init_completion(&new_sa->done);

	err = eip93_ipsec_parse_flags(x, &new_sa->flags);
	if (err)
		goto free_sa;

	new_sa->sa_record = kzalloc(sizeof(*new_sa->sa_record), GFP_KERNEL);
	if (!new_sa->sa_record) {
		err = -ENOMEM;
		goto free_sa;
	}

	eip93_ipsec_init_sa_record(new_sa, x);

	enckeylen = x->ealg->alg_key_len / BITS_PER_BYTE;
	memcpy(new_sa->sa_record->sa_key, x->ealg->alg_key, enckeylen);

	authkeylen = x->aalg->alg_key_len / BITS_PER_BYTE;
	err = eip93_ipsec_hmac_setkey(new_sa->flags, x->aalg->alg_key,
				      authkeylen,
				      new_sa->sa_record->sa_i_digest,
				      new_sa->sa_record->sa_o_digest);
	if (err)
		goto free_record;

	new_sa->sa_record_base = dma_map_single(eip93->dev, new_sa->sa_record,
						sizeof(*new_sa->sa_record),
						DMA_TO_DEVICE);
	if (dma_mapping_error(eip93->dev, new_sa->sa_record_base)) {
		err = -ENOMEM;
		goto free_record;
	}

	spin_lock_bh(&ipsec->lock);
	if (ipsec->dead) {
		spin_unlock_bh(&ipsec->lock);
		err = -EOPNOTSUPP;
		goto unmap_record;
	}
	list_add_tail(&new_sa->node, &ipsec->sa_list);
	spin_unlock_bh(&ipsec->lock);

	*sa = new_sa;

	return 0;

unmap_record:
	dma_unmap_single(eip93->dev, new_sa->sa_record_base,
			 sizeof(*new_sa->sa_record), DMA_TO_DEVICE);
free_record:
	kfree_sensitive(new_sa->sa_record);
free_sa:
	kfree(new_sa);
put_ipsec:
	eip93_ipsec_put(ipsec);
	return err;
}
EXPORT_SYMBOL_GPL(eip93_ipsec_state_add);

void eip93_ipsec_state_delete(struct eip93_ipsec_sa *sa)
{
	if (!sa)
		return;

	spin_lock_bh(&sa->ipsec->lock);
	spin_lock(&sa->lock);
	sa->dead = true;
	list_del_init(&sa->node);
	spin_unlock(&sa->lock);
	spin_unlock_bh(&sa->ipsec->lock);

	eip93_ipsec_sa_put(sa);
	wait_for_completion(&sa->done);

	dma_unmap_single(sa->ipsec->eip93->dev, sa->sa_record_base,
			 sizeof(*sa->sa_record), DMA_TO_DEVICE);
	kfree_sensitive(sa->sa_record);
	eip93_ipsec_put(sa->ipsec);
	kfree(sa);
}
EXPORT_SYMBOL_GPL(eip93_ipsec_state_delete);

void eip93_ipsec_state_advance_esn(struct eip93_ipsec_sa *sa,
				   struct xfrm_state *x)
{
	u32 seq_hi = 0;

	if (!sa || !x || !sa->esn || !x->replay_esn)
		return;

	if (x->xso.dir == XFRM_DEV_OFFLOAD_IN)
		seq_hi = x->replay_esn->seq_hi;
	else if (x->xso.dir == XFRM_DEV_OFFLOAD_OUT)
		seq_hi = x->replay_esn->oseq_hi;

	spin_lock_bh(&sa->lock);
	if (!sa->dead) {
		sa->sa_record->sa_seqnum[1] = seq_hi;
		dma_sync_single_for_device(sa->ipsec->eip93->dev,
					   sa->sa_record_base,
					   sizeof(*sa->sa_record),
					   DMA_TO_DEVICE);
	}
	spin_unlock_bh(&sa->lock);
}
EXPORT_SYMBOL_GPL(eip93_ipsec_state_advance_esn);

int eip93_ipsec_xmit(struct eip93_ipsec_sa *sa, struct sk_buff *skb,
		     unsigned int esp_offset, eip93_ipsec_complete_t complete,
		     void *data)
{
	struct eip93_descriptor cdesc = {};
	struct eip93_ipsec_request *req;
	struct xfrm_offload *xo;
	unsigned int payload_len;
	unsigned int crypt_len;
	unsigned int dma_len;
	unsigned int tailen;
	int err;

	if (!sa || !complete || !eip93_ipsec_sa_get(sa))
		return -EOPNOTSUPP;

	if (skb_is_nonlinear(skb)) {
		err = -EINVAL;
		goto put_sa;
	}

	if (skb->len <= esp_offset + sizeof(struct ip_esp_hdr) + sa->ivsize) {
		err = -EINVAL;
		goto put_sa;
	}

	xo = xfrm_offload(skb);
	if (!xo) {
		err = -EINVAL;
		goto put_sa;
	}

	tailen = xo->esp_tx_tailen;
	if (tailen) {
		payload_len = skb->len - esp_offset - sizeof(struct ip_esp_hdr) -
			      sa->ivsize;
		dma_len = skb->len + tailen;
		if (tailen > skb_tailroom(skb) || dma_len < skb->len) {
			err = -ENOMEM;
			goto put_sa;
		}
	} else {
		u8 *trail;
		u8 padlen;

		if (skb->len <= esp_offset + sizeof(struct ip_esp_hdr) +
					sa->ivsize + sa->authsize) {
			err = -EINVAL;
			goto put_sa;
		}

		crypt_len = skb->len - esp_offset - sizeof(struct ip_esp_hdr) -
			    sa->ivsize - sa->authsize;
		if (crypt_len < 2) {
			err = -EINVAL;
			goto put_sa;
		}

		trail = skb_tail_pointer(skb) - sa->authsize - 2;
		padlen = trail[0];
		if (crypt_len < padlen + 2) {
			err = -EINVAL;
			goto put_sa;
		}

		payload_len = crypt_len - padlen - 2;
		dma_len = skb->len;
	}
	if (payload_len > FIELD_MAX(EIP93_PE_LENGTH_LENGTH)) {
		err = -EINVAL;
		goto put_sa;
	}

	req = kmalloc(sizeof(*req), GFP_ATOMIC);
	if (!req) {
		err = -ENOMEM;
		goto put_sa;
	}

	req->sa = sa;
	req->skb = skb;
	INIT_LIST_HEAD(&req->node);
	refcount_set(&req->refcnt, 1);
	req->complete = complete;
	req->data = data;
	req->dma_len = dma_len;
	req->dma_dir = DMA_BIDIRECTIONAL;
	req->dma = dma_map_single(sa->ipsec->eip93->dev, skb->data,
				  req->dma_len, req->dma_dir);
	if (dma_mapping_error(sa->ipsec->eip93->dev, req->dma)) {
		err = -ENOMEM;
		goto free_req;
	}

	cdesc.pe_ctrl_stat_word =
		FIELD_PREP(EIP93_PE_CTRL_PE_READY_DES_TRING_OWN,
			   EIP93_PE_CTRL_HOST_READY) |
		FIELD_PREP(EIP93_PE_CTRL_PE_PAD_CTRL_STAT,
			   EIP93_IPSEC_PAD_ALIGN) |
			FIELD_PREP(EIP93_PE_CTRL_PE_PAD_VALUE, xo->proto) |
		EIP93_PE_CTRL_PE_HASH_FINAL;
	cdesc.src_addr = (u32 __force)req->dma + esp_offset +
			 sizeof(struct ip_esp_hdr) + sa->ivsize;
	cdesc.dst_addr = (u32 __force)req->dma + esp_offset;
	cdesc.sa_addr = sa->sa_record_base;
	/*
	 * EIP93 ESP protocol-out mode wants the plaintext payload length. It
	 * generates ESP padding, next-header and ICV itself when tailroom was
	 * reserved instead of filled by the generic ESP path.
	 */
	cdesc.pe_length_word = FIELD_PREP(EIP93_PE_LENGTH_HOST_PE_READY,
					  EIP93_PE_LENGTH_HOST_READY) |
			       FIELD_PREP(EIP93_PE_LENGTH_LENGTH, payload_len);

	err = eip93_ipsec_submit(req, &cdesc);
	if (err == -EINPROGRESS)
		return err;

	dma_unmap_single(sa->ipsec->eip93->dev, req->dma, req->dma_len,
			 req->dma_dir);
free_req:
	eip93_ipsec_request_put(req);
put_sa:
	eip93_ipsec_sa_put(sa);
	return err;
}
EXPORT_SYMBOL_GPL(eip93_ipsec_xmit);

int eip93_ipsec_receive(struct eip93_ipsec_sa *sa, struct sk_buff *skb,
			unsigned int packet_len,
			eip93_ipsec_complete_t complete, void *data)
{
	struct eip93_descriptor cdesc = {};
	struct eip93_ipsec_request *req;
	int err;

	if (!sa || !complete || !eip93_ipsec_sa_get(sa))
		return -EOPNOTSUPP;

	if (skb_is_nonlinear(skb)) {
		err = -EINVAL;
		goto put_sa;
	}

	req = kmalloc(sizeof(*req), GFP_ATOMIC);
	if (!req) {
		err = -ENOMEM;
		goto put_sa;
	}

	req->sa = sa;
	req->skb = skb;
	INIT_LIST_HEAD(&req->node);
	refcount_set(&req->refcnt, 1);
	req->complete = complete;
	req->data = data;
	if (!packet_len || packet_len > skb->len ||
	    packet_len > FIELD_MAX(EIP93_PE_LENGTH_LENGTH)) {
		err = -EINVAL;
		goto free_req;
	}

	req->dma_len = packet_len;
	req->dma_dir = DMA_BIDIRECTIONAL;
	req->dma = dma_map_single(sa->ipsec->eip93->dev, skb->data,
				  req->dma_len, req->dma_dir);
	if (dma_mapping_error(sa->ipsec->eip93->dev, req->dma)) {
		err = -ENOMEM;
		goto free_req;
	}

	cdesc.pe_ctrl_stat_word =
		FIELD_PREP(EIP93_PE_CTRL_PE_READY_DES_TRING_OWN,
			   EIP93_PE_CTRL_HOST_READY) |
		FIELD_PREP(EIP93_PE_CTRL_PE_PAD_CTRL_STAT,
			   EIP93_IPSEC_PAD_ALIGN) |
		EIP93_PE_CTRL_PE_HASH_FINAL;
	cdesc.src_addr = (u32 __force)req->dma;
	cdesc.dst_addr = (u32 __force)req->dma;
	cdesc.sa_addr = sa->sa_record_base;
	cdesc.pe_length_word = FIELD_PREP(EIP93_PE_LENGTH_HOST_PE_READY,
					  EIP93_PE_LENGTH_HOST_READY) |
			       FIELD_PREP(EIP93_PE_LENGTH_LENGTH, req->dma_len);

	err = eip93_ipsec_submit(req, &cdesc);
	if (err == -EINPROGRESS)
		return err;

	dma_unmap_single(sa->ipsec->eip93->dev, req->dma, req->dma_len,
			 req->dma_dir);
free_req:
	eip93_ipsec_request_put(req);
put_sa:
	eip93_ipsec_sa_put(sa);
	return err;
}
EXPORT_SYMBOL_GPL(eip93_ipsec_receive);
