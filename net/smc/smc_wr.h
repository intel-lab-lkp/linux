/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 * Work Requests exploiting Infiniband API
 *
 * Copyright IBM Corp. 2016
 *
 * Author(s):  Steffen Maier <maier@linux.vnet.ibm.com>
 */

#ifndef SMC_WR_H
#define SMC_WR_H

#include <linux/atomic.h>
#include <rdma/ib_verbs.h>
#include <asm/div64.h>

#include "smc.h"
#include "smc_core.h"

#define SMC_WR_TX_WAIT_FREE_SLOT_TIME	(10 * HZ)

#define SMC_WR_TX_SIZE 44 /* actual size of wr_send data (<=SMC_WR_BUF_SIZE) */

#define SMC_WR_TX_PEND_PRIV_SIZE 32

struct smc_wr_tx_pend_priv {
	u8			priv[SMC_WR_TX_PEND_PRIV_SIZE];
};

typedef void (*smc_wr_tx_handler)(struct smc_wr_tx_pend_priv *,
				  struct smc_link *,
				  enum ib_wc_status);

typedef bool (*smc_wr_tx_filter)(struct smc_wr_tx_pend_priv *,
				 unsigned long);

typedef void (*smc_wr_tx_dismisser)(struct smc_wr_tx_pend_priv *);

struct smc_wr_rx_handler {
	struct hlist_node	list;	/* hash table collision resolution */
	void			(*handler)(struct ib_wc *, void *);
	u8			type;
};

static inline bool smc_wr_tx_link_hold(struct smc_link *link)
{
	/* We still keep this check, although percpu_ref_tryget_live()
	 * is the real safety guarantee.
	 * It provides a quick reject path, but nothing more than that.
	 */
	if (!smc_link_sendable(link))
		return false;
	return percpu_ref_tryget_live(&link->wr_tx_refs);
}

static inline void smc_wr_tx_link_put(struct smc_link *link)
{
	percpu_ref_put(&link->wr_tx_refs);
}

static inline void smc_wr_drain_qp(struct smc_link *lnk)
{
	if (lnk->qp_attr.cur_qp_state != IB_QPS_RESET)
		ib_drain_qp(lnk->roce_qp);
}

static inline void smc_wr_wakeup_tx_wait(struct smc_link *lnk)
{
	wake_up_all(&lnk->wr_tx_wait);
}

static inline void smc_wr_wakeup_reg_wait(struct smc_link *lnk)
{
	wake_up(&lnk->wr_reg_wait);
}

/* post a new receive work request to fill a completed old work request entry */
static inline int smc_wr_rx_post(struct smc_link *link, struct ib_cqe *cqe)
{
	struct smc_ib_recv_wr *recv_wr;

	recv_wr = container_of(cqe, struct smc_ib_recv_wr, cqe);
	return ib_post_recv(link->roce_qp, &recv_wr->wr, NULL);
}

int smc_wr_create_link(struct smc_link *lnk);
int smc_wr_alloc_link_mem(struct smc_link *lnk);
int smc_wr_alloc_lgr_mem(struct smc_link_group *lgr);
void smc_wr_stop_link(struct smc_link *lnk);
void smc_wr_free_link(struct smc_link *lnk);
void smc_wr_free_link_mem(struct smc_link *lnk);
void smc_wr_free_lgr_mem(struct smc_link_group *lgr);
void smc_wr_remember_qp_attr(struct smc_link *lnk);

int smc_wr_tx_get_free_slot(struct smc_link *link, smc_wr_tx_handler handler,
			    struct smc_wr_buf **wr_buf,
			    struct smc_rdma_wr **wrs,
			    struct smc_wr_tx_pend_priv **wr_pend_priv);
int smc_wr_tx_get_v2_slot(struct smc_link *link,
			  smc_wr_tx_handler handler,
			  struct smc_wr_v2_buf **wr_buf,
			  struct smc_wr_tx_pend_priv **wr_pend_priv);
int smc_wr_tx_put_slot(struct smc_link *link,
		       struct smc_wr_tx_pend_priv *wr_pend_priv);
int smc_wr_tx_send(struct smc_link *link,
		   struct smc_wr_tx_pend_priv *wr_pend_priv);
int smc_wr_tx_v2_send(struct smc_link *link,
		      struct smc_wr_tx_pend_priv *priv, int len);
int smc_wr_tx_send_wait(struct smc_link *link, struct smc_wr_tx_pend_priv *priv,
			unsigned long timeout);
void smc_wr_tx_wait_no_pending_sends(struct smc_link *link);

int smc_wr_rx_register_handler(struct smc_wr_rx_handler *handler);
int smc_wr_rx_post_init(struct smc_link *link);
int smc_wr_reg_send(struct smc_link *link, struct ib_mr *mr);

void smc_wr_init_cqes(struct smc_link *lnk);

#endif /* SMC_WR_H */
