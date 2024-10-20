/* SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2024, Pawel Dembicki <paweldembicki@gmail.com>
 */

/* Included by drivers/net/dsa/vitesse-vsc73xx.h and net/dsa/tag_vsc73xx_8021q.c */

#ifndef _NET_DSA_VSC73XX_H
#define _NET_DSA_VSC73XX_H

struct vsc73xx_deferred_xmit_work {
	struct dsa_port *dp;
	struct sk_buff *skb;
	struct kthread_work work;
};

struct vsc73xx_8021q_tagger_data {
	void (*xmit_work_fn)(struct kthread_work *work);
};

#endif /* _NET_DSA_VSC73XX_H */
