/* SPDX-License-Identifier: GPL-2.0 */
/*
 * EIP93 IPsec offload API
 *
 * Copyright (c) 2026 Jihong Min <hurryman2212@gmail.com>
 */
#ifndef _CRYPTO_EIP93_IPSEC_H
#define _CRYPTO_EIP93_IPSEC_H

#include <linux/bits.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct netlink_ext_ack;
struct notifier_block;
struct sk_buff;
struct xfrm_state;

struct eip93_ipsec;
struct eip93_ipsec_sa;

struct eip93_ipsec_result {
	unsigned int packet_len;
	u8 nexthdr;
};

enum eip93_ipsec_feature {
	EIP93_IPSEC_FEATURE_ESP = BIT(0),
	EIP93_IPSEC_FEATURE_GSO_ESP = BIT(1),
	EIP93_IPSEC_FEATURE_HW_ESP_TX_CSUM = BIT(2),
};

enum eip93_ipsec_event {
	EIP93_IPSEC_EVENT_REMOVE,
	EIP93_IPSEC_EVENT_RESET,
	EIP93_IPSEC_EVENT_DMA_ERROR,
	EIP93_IPSEC_EVENT_CAPABILITY_LOSS,
};

typedef void (*eip93_ipsec_complete_t)(void *data, int err,
				       struct eip93_ipsec_result result);

#if IS_REACHABLE(CONFIG_CRYPTO_DEV_EIP93) && \
	IS_ENABLED(CONFIG_CRYPTO_DEV_EIP93_IPSEC)
struct eip93_ipsec *eip93_ipsec_get(struct device *consumer);
void eip93_ipsec_put(struct eip93_ipsec *ipsec);
bool eip93_ipsec_available(struct eip93_ipsec *ipsec);
u32 eip93_ipsec_features(struct eip93_ipsec *ipsec);
int eip93_ipsec_register_notifier(struct notifier_block *nb);
void eip93_ipsec_unregister_notifier(struct notifier_block *nb);
int eip93_ipsec_state_add(struct eip93_ipsec *ipsec, struct xfrm_state *x,
			  struct netlink_ext_ack *extack,
			  struct eip93_ipsec_sa **sa);
void eip93_ipsec_state_delete(struct eip93_ipsec_sa *sa);
void eip93_ipsec_state_advance_esn(struct eip93_ipsec_sa *sa,
				   struct xfrm_state *x);
int eip93_ipsec_xmit(struct eip93_ipsec_sa *sa, struct sk_buff *skb,
		     unsigned int esp_offset, eip93_ipsec_complete_t complete,
		     void *data);
int eip93_ipsec_receive(struct eip93_ipsec_sa *sa, struct sk_buff *skb,
			unsigned int packet_len,
			eip93_ipsec_complete_t complete, void *data);
#else
static inline struct eip93_ipsec *eip93_ipsec_get(struct device *consumer)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline void eip93_ipsec_put(struct eip93_ipsec *ipsec)
{
}

static inline bool eip93_ipsec_available(struct eip93_ipsec *ipsec)
{
	return false;
}

static inline u32 eip93_ipsec_features(struct eip93_ipsec *ipsec)
{
	return 0;
}

static inline int eip93_ipsec_register_notifier(struct notifier_block *nb)
{
	return 0;
}

static inline void eip93_ipsec_unregister_notifier(struct notifier_block *nb)
{
}

static inline int eip93_ipsec_state_add(struct eip93_ipsec *ipsec,
					struct xfrm_state *x,
					struct netlink_ext_ack *extack,
					struct eip93_ipsec_sa **sa)
{
	if (sa)
		*sa = NULL;

	return -EOPNOTSUPP;
}

static inline void eip93_ipsec_state_delete(struct eip93_ipsec_sa *sa)
{
}

static inline void eip93_ipsec_state_advance_esn(struct eip93_ipsec_sa *sa,
						 struct xfrm_state *x)
{
}

static inline int eip93_ipsec_xmit(struct eip93_ipsec_sa *sa,
				   struct sk_buff *skb, unsigned int esp_offset,
				   eip93_ipsec_complete_t complete, void *data)
{
	return -EOPNOTSUPP;
}

static inline int eip93_ipsec_receive(struct eip93_ipsec_sa *sa,
				      struct sk_buff *skb,
				      unsigned int packet_len,
				      eip93_ipsec_complete_t complete,
				      void *data)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _CRYPTO_EIP93_IPSEC_H */
