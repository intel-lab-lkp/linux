/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2024 Intel Corporation */

#ifndef _IDPF_VIRTCHNL_H_
#define _IDPF_VIRTCHNL_H_

#define IDPF_VC_XN_MIN_TIMEOUT_MSEC	2000
#define IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC	(60 * 1000)

struct idpf_adapter;
struct idpf_netdev_priv;
struct idpf_vec_regs;
struct idpf_vport;
struct idpf_vport_max_q;
struct idpf_vport_user_config_data;

int idpf_init_dflt_mbx(struct idpf_adapter *adapter);
void idpf_deinit_dflt_mbx(struct idpf_adapter *adapter);
int idpf_vc_core_init(struct idpf_adapter *adapter);
void idpf_vc_core_deinit(struct idpf_adapter *adapter);

int idpf_get_reg_intr_vecs(struct idpf_adapter *adapter,
			   struct idpf_vec_regs *reg_vals);
int idpf_queue_reg_init(struct idpf_vport *vport,
			struct idpf_q_vec_rsrc *rsrc,
			struct idpf_queue_id_reg_info *chunks);
int idpf_vport_queue_ids_init(struct idpf_vport *vport,
			      struct idpf_q_vec_rsrc *rsrc,
			      struct idpf_queue_id_reg_info *chunks);

bool idpf_vport_is_cap_ena(struct idpf_vport *vport, u16 flag);
bool idpf_sideband_flow_type_ena(struct idpf_vport *vport, u32 flow_type);
bool idpf_sideband_action_ena(struct idpf_vport *vport,
			      struct ethtool_rx_flow_spec *fsp);
unsigned int idpf_fsteer_max_rules(struct idpf_vport *vport);

void idpf_recv_event_msg(struct libie_ctlq_ctx *ctx,
			 struct libie_ctlq_msg *ctlq_msg);
int idpf_send_mb_msg(struct idpf_adapter *adapter,
		     struct libie_ctlq_xn_send_params *xn_params,
		     void *send_buf, size_t send_buf_size);

int idpf_vport_init(struct idpf_vport *vport, struct idpf_vport_max_q *max_q);
u32 idpf_get_vport_id(struct idpf_vport *vport);
int idpf_send_create_vport_msg(struct idpf_adapter *adapter,
			       struct idpf_vport_max_q *max_q);
int idpf_send_destroy_vport_msg(struct idpf_adapter *adapter, u32 vport_id);
int idpf_send_enable_vport_msg(struct idpf_adapter *adapter, u32 vport_id);
int idpf_send_disable_vport_msg(struct idpf_adapter *adapter, u32 vport_id);

int idpf_vport_adjust_qs(struct idpf_vport *vport,
			 struct idpf_q_vec_rsrc *rsrc);
int idpf_vport_alloc_max_qs(struct idpf_adapter *adapter,
			    struct idpf_vport_max_q *max_q);
void idpf_vport_dealloc_max_qs(struct idpf_adapter *adapter,
			       struct idpf_vport_max_q *max_q);
int idpf_send_add_queues_msg(struct idpf_adapter *adapter,
			     struct idpf_vport_config *vport_config,
			     struct idpf_q_vec_rsrc *rsrc,
			     u32 vport_id);
int idpf_send_delete_queues_msg(struct idpf_adapter *adapter,
				struct idpf_queue_id_reg_info *chunks,
				u32 vport_id);
int idpf_send_enable_queues_msg(struct idpf_vport *vport,
				struct idpf_queue_id_reg_info *chunks);
int idpf_send_disable_queues_msg(struct idpf_vport *vport,
				 struct idpf_q_vec_rsrc *rsrc,
				 struct idpf_queue_id_reg_info *chunks);
int idpf_send_config_queues_msg(struct idpf_adapter *adapter,
				struct idpf_q_vec_rsrc *rsrc,
				u32 vport_id, bool rsc_ena);

int idpf_vport_alloc_vec_indexes(struct idpf_vport *vport,
				 struct idpf_q_vec_rsrc *rsrc);
int idpf_get_vec_ids(struct idpf_adapter *adapter,
		     u16 *vecids, int num_vecids,
		     struct virtchnl2_vector_chunks *chunks);
int idpf_send_alloc_vectors_msg(struct idpf_adapter *adapter, u16 num_vectors);
int idpf_send_dealloc_vectors_msg(struct idpf_adapter *adapter);
int idpf_send_map_unmap_queue_vector_msg(struct idpf_adapter *adapter,
					 struct idpf_q_vec_rsrc *rsrc,
					 u32 vport_id,
					 bool map);

int idpf_add_del_mac_filters(struct idpf_adapter *adapter,
			     struct idpf_vport_config *vport_config,
			     u32 vport_id, bool add, bool async);
int idpf_set_promiscuous(struct idpf_adapter *adapter,
			 struct idpf_vport_user_config_data *config_data,
			 u32 vport_id);
int idpf_check_supported_desc_ids(struct idpf_vport *vport);
int idpf_send_ena_dis_loopback_msg(struct idpf_adapter *adapter, u32 vport_id,
				   bool loopback_ena);
int idpf_send_get_stats_msg(struct idpf_netdev_priv *np,
			    struct idpf_port_stats *port_stats);
int idpf_send_set_sriov_vfs_msg(struct idpf_adapter *adapter, u16 num_vfs);
int idpf_send_get_set_rss_key_msg(struct idpf_adapter *adapter,
				  struct idpf_rss_data *rss_data,
				  u32 vport_id, bool get);
int idpf_send_get_set_rss_lut_msg(struct idpf_adapter *adapter,
				  struct idpf_rss_data *rss_data,
				  u32 vport_id, bool get);

#endif /* _IDPF_VIRTCHNL_H_ */
