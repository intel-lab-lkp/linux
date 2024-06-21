/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * NXP Wireless LAN device driver: WMM
 *
 * Copyright 2011-2024 NXP
 */

#ifndef _NXPWIFI_WMM_H_
#define _NXPWIFI_WMM_H_

enum ieee_types_wmm_aciaifsn_bitmasks {
	NXPWIFI_AIFSN = (BIT(0) | BIT(1) | BIT(2) | BIT(3)),
	NXPWIFI_ACM = BIT(4),
	NXPWIFI_ACI = (BIT(5) | BIT(6)),
};

enum ieee_types_wmm_ecw_bitmasks {
	NXPWIFI_ECW_MIN = (BIT(0) | BIT(1) | BIT(2) | BIT(3)),
	NXPWIFI_ECW_MAX = (BIT(4) | BIT(5) | BIT(6) | BIT(7)),
};

extern const u16 nxpwifi_1d_to_wmm_queue[];
extern const u8 tos_to_tid_inv[];

/* This function retrieves the TID of the given RA list.
 */
static inline int
nxpwifi_get_tid(struct nxpwifi_ra_list_tbl *ptr)
{
	struct sk_buff *skb;

	if (skb_queue_empty(&ptr->skb_head))
		return 0;

	skb = skb_peek(&ptr->skb_head);

	return skb->priority;
}

/* This function checks if a RA list is empty or not.
 */
static inline u8
nxpwifi_wmm_is_ra_list_empty(struct list_head *ra_list_hhead)
{
	struct nxpwifi_ra_list_tbl *ra_list;
	int is_list_empty;

	list_for_each_entry(ra_list, ra_list_hhead, list) {
		is_list_empty = skb_queue_empty(&ra_list->skb_head);
		if (!is_list_empty)
			return false;
	}

	return true;
}

void nxpwifi_wmm_add_buf_txqueue(struct nxpwifi_private *priv,
				 struct sk_buff *skb);
void nxpwifi_wmm_add_buf_bypass_txqueue(struct nxpwifi_private *priv,
					struct sk_buff *skb);
void nxpwifi_ralist_add(struct nxpwifi_private *priv, const u8 *ra);
void nxpwifi_rotate_priolists(struct nxpwifi_private *priv,
			      struct nxpwifi_ra_list_tbl *ra, int tid);

int nxpwifi_wmm_lists_empty(struct nxpwifi_adapter *adapter);
int nxpwifi_bypass_txlist_empty(struct nxpwifi_adapter *adapter);
void nxpwifi_wmm_process_tx(struct nxpwifi_adapter *adapter);
void nxpwifi_process_bypass_tx(struct nxpwifi_adapter *adapter);
int nxpwifi_is_ralist_valid(struct nxpwifi_private *priv,
			    struct nxpwifi_ra_list_tbl *ra_list, int tid);

u8 nxpwifi_wmm_compute_drv_pkt_delay(struct nxpwifi_private *priv,
				     const struct sk_buff *skb);
void nxpwifi_wmm_init(struct nxpwifi_adapter *adapter);

u32 nxpwifi_wmm_process_association_req(struct nxpwifi_private *priv,
					u8 **assoc_buf,
					struct ieee_types_wmm_parameter *wmmie,
					struct ieee80211_ht_cap *htcap);

void nxpwifi_wmm_setup_queue_priorities(struct nxpwifi_private *priv,
					struct ieee_types_wmm_parameter *wmm_ie);
void nxpwifi_wmm_setup_ac_downgrade(struct nxpwifi_private *priv);
int nxpwifi_ret_wmm_get_status(struct nxpwifi_private *priv,
			       const struct host_cmd_ds_command *resp);
struct nxpwifi_ra_list_tbl *
nxpwifi_wmm_get_queue_raptr(struct nxpwifi_private *priv, u8 tid,
			    const u8 *ra_addr);
u8 nxpwifi_wmm_downgrade_tid(struct nxpwifi_private *priv, u32 tid);
void nxpwifi_update_ralist_tx_pause(struct nxpwifi_private *priv, u8 *mac,
				    u8 tx_pause);

struct nxpwifi_ra_list_tbl *nxpwifi_wmm_get_ralist_node(struct nxpwifi_private
					*priv, u8 tid, const u8 *ra_addr);
#endif /* !_NXPWIFI_WMM_H_ */
