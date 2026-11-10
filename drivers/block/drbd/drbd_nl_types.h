/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Internal representation of DRBD configuration options, parameters,
 * state information and statistics as exchanged over generic netlink.
 *
 * These structs are shared by every netlink dialect the kernel module
 * serves (the legacy "drbd" family and "drbd2"); the per-dialect
 * marshalling code translates between them and the wire format. They
 * used to be generated from linux/drbd_genl_ynl.yaml; keep field names
 * and types in sync with the specs when adding attributes.
 */
#ifndef __DRBD_NL_TYPES_H
#define __DRBD_NL_TYPES_H

#include <linux/types.h>
#include <linux/drbd.h>

struct drbd_nl_cfg_reply {
	char info_text[0];
	__u32 info_text_len;
};

struct drbd_nl_cfg_context {
	__u32 ctx_peer_node_id;
	__u32 ctx_volume;
	char ctx_resource_name[128];
	__u32 ctx_resource_name_len;
	char ctx_my_addr[128];
	__u32 ctx_my_addr_len;
	char ctx_peer_addr[128];
	__u32 ctx_peer_addr_len;
	char ctx_conn_name[SHARED_SECRET_MAX];
	__u32 ctx_conn_name_len;
};

struct drbd_disk_conf {
	char backing_dev[128];
	__u32 backing_dev_len;
	char meta_dev[128];
	__u32 meta_dev_len;
	__s32 meta_dev_idx;
	__u64 disk_size;
	__u32 on_io_error;
	__s32 resync_after;
	__u32 al_extents;
	unsigned char disk_barrier;
	unsigned char disk_flushes;
	unsigned char disk_drain;
	unsigned char md_flushes;
	__u32 disk_timeout;
	__u32 read_balancing;
	__u32 unplug_watermark;
	__u32 rs_discard_granularity;
	unsigned char al_updates;
	unsigned char discard_zeroes_if_aligned;
	unsigned char disable_write_same;
	unsigned char d_bitmap;
};

struct drbd_res_opts {
	char cpu_mask[DRBD_CPU_MASK_SIZE];
	__u32 cpu_mask_len;
	__u32 on_no_data;
	unsigned char auto_promote;
	__u32 node_id;
	__u32 peer_ack_window;
	__u32 twopc_timeout;
	__u32 twopc_retry_timeout;
	__u32 peer_ack_delay;
	__u32 auto_promote_timeout;
	__u32 nr_requests;
	__s32 quorum;
	__u32 on_no_quorum;
	__s32 quorum_min_redundancy;
	__u32 on_susp_primary_outdated;
	unsigned char drbd8_compat_mode;
	unsigned char explicit_drbd8_compat;
};

struct drbd_net_conf {
	char shared_secret[SHARED_SECRET_MAX];
	__u32 shared_secret_len;
	char cram_hmac_alg[SHARED_SECRET_MAX];
	__u32 cram_hmac_alg_len;
	char integrity_alg[SHARED_SECRET_MAX];
	__u32 integrity_alg_len;
	char verify_alg[SHARED_SECRET_MAX];
	__u32 verify_alg_len;
	char csums_alg[SHARED_SECRET_MAX];
	__u32 csums_alg_len;
	__u32 wire_protocol;
	__u32 connect_int;
	__u32 timeout;
	__u32 ping_int;
	__u32 ping_timeo;
	__u32 sndbuf_size;
	__u32 rcvbuf_size;
	__u32 ko_count;
	__u32 max_epoch_size;
	__u32 after_sb_0p;
	__u32 after_sb_1p;
	__u32 after_sb_2p;
	__u32 rr_conflict;
	__u32 on_congestion;
	__u32 cong_fill;
	__u32 cong_extents;
	unsigned char two_primaries;
	unsigned char tcp_cork;
	unsigned char always_asbp;
	unsigned char use_rle;
	__u32 fencing_policy;
	char name[SHARED_SECRET_MAX];
	__u32 name_len;
	unsigned char csums_after_crash_only;
	__u32 sock_check_timeo;
	char transport_name[SHARED_SECRET_MAX];
	__u32 transport_name_len;
	__u32 max_buffers;
	unsigned char allow_remote_read;
	unsigned char tls;
	__s32 tls_privkey;
	__s32 tls_certificate;
	__s32 tls_keyring;
	unsigned char load_balance_paths;
	__u32 rdma_ctrl_rcvbuf_size;
	__u32 rdma_ctrl_sndbuf_size;
};

struct drbd_set_role_parms {
	unsigned char force;
};

struct drbd_resize_parms {
	__u64 resize_size;
	unsigned char resize_force;
	unsigned char no_resync;
	__u32 al_stripes;
	__u32 al_stripe_size;
};

struct drbd_start_ov_parms {
	__u64 ov_start_sector;
	__u64 ov_stop_sector;
};

struct drbd_new_c_uuid_parms {
	unsigned char clear_bm;
	unsigned char force_resync;
};

struct drbd_timeout_parms {
	__u32 timeout_type;
};

struct drbd_disconnect_parms {
	unsigned char force_disconnect;
};

struct drbd_detach_parms {
	unsigned char force_detach;
	unsigned char intentional_diskless_detach;
};

struct drbd_device_conf {
	__u32 max_bio_size;
	unsigned char intentional_diskless;
	__u32 block_size;
	__u32 discard_granularity;
};

struct drbd_resource_info {
	__u32 res_role;
	unsigned char res_susp;
	unsigned char res_susp_nod;
	unsigned char res_susp_fen;
	unsigned char res_susp_quorum;
	unsigned char res_fail_io;
#ifdef CONFIG_DRBD_COMPAT_84
	/*
	 * The pre-transition values of the four fields above, for the v1
	 * dialect's fused DRBD_EVENT (drbd_nl_84.c's compat84_emit_event()):
	 * 8.4's ST-prev/ST-new event lines need a real "before" state, not
	 * just the "after" state every other field here already carries.
	 * Filled only at notify_resource_state_change()'s NOTIFY_CHANGE call
	 * site (drbd_state.c); left unset (and unread) everywhere else,
	 * including every notify_resource_state() call the v1 dialect never
	 * sees a fused event for.
	 */
	__u32 old_res_role;
	unsigned char old_res_susp;
	unsigned char old_res_susp_nod;
	unsigned char old_res_susp_fen;
#endif
};

struct drbd_device_info {
	__u32 dev_disk_state;
	unsigned char is_intentional_diskless;
	unsigned char dev_has_quorum;
	unsigned char dev_is_open;
	char backing_dev_path[128];
	__u32 backing_dev_path_len;
#ifdef CONFIG_DRBD_COMPAT_84
	/* dev_disk_state's pre-transition value; see resource_info above. */
	__u32 old_dev_disk_state;
#endif
};

struct drbd_connection_info {
	__u32 conn_connection_state;
	__u32 conn_role;
#ifdef CONFIG_DRBD_COMPAT_84
	/* Pre-transition values of the two fields above; see resource_info
	 * above.
	 */
	__u32 old_conn_connection_state;
	__u32 old_conn_role;
#endif
};

struct drbd_peer_device_info {
	__u32 peer_repl_state;
	__u32 peer_disk_state;
	__u32 peer_resync_susp_user;
	__u32 peer_resync_susp_peer;
	__u32 peer_resync_susp_dependency;
	unsigned char peer_is_intentional_diskless;
	__u32 peer_resync_susp_max_parallel;
#ifdef CONFIG_DRBD_COMPAT_84
	/* peer_repl_state's and peer_disk_state's pre-transition values; see
	 * resource_info above. old_peer_resync_susp_user/_peer are the exact
	 * pre-transition values of the two fields above them; old_peer_
	 * resync_susp_dependency approximates peer_resync_susp_dependency's
	 * combined derivation (resync_susp_dependency || resync_susp_other_c
	 * || (sync-source with an inconsistent local disk)) with only its
	 * first two terms, since the third needs the sibling device's
	 * pre-transition disk state, not reachable from a peer-device-only
	 * snapshot without widening the notification callback's signature.
	 */
	__u32 old_peer_repl_state;
	__u32 old_peer_disk_state;
	__u32 old_peer_resync_susp_user;
	__u32 old_peer_resync_susp_peer;
	__u32 old_peer_resync_susp_dependency;
#endif
};

struct drbd_resource_statistics {
	__u32 res_stat_write_ordering;
};

struct drbd_device_statistics {
	__u64 dev_size;
	__u64 dev_read;
	__u64 dev_write;
	__u64 dev_al_writes;
	__u64 dev_bm_writes;
	__u32 dev_upper_pending;
	__u32 dev_lower_pending;
	unsigned char dev_upper_blocked;
	unsigned char dev_lower_blocked;
	unsigned char dev_al_suspended;
	__u64 dev_exposed_data_uuid;
	__u64 dev_current_uuid;
	__u32 dev_disk_flags;
	char history_uuids[HISTORY_UUIDS_SIZE];
	__u32 history_uuids_len;
};

struct drbd_connection_statistics {
	unsigned char conn_congested;
	__u64 ap_in_flight;
	__u64 rs_in_flight;
};

struct drbd_peer_device_statistics {
	__u64 peer_dev_received;
	__u64 peer_dev_sent;
	__u32 peer_dev_pending;
	__u32 peer_dev_unacked;
	__u64 peer_dev_out_of_sync;
	__u64 peer_dev_resync_failed;
	__u64 peer_dev_bitmap_uuid;
	__u32 peer_dev_flags;
	__u64 peer_dev_rs_total;
	__u64 peer_dev_ov_start_sector;
	__u64 peer_dev_ov_stop_sector;
	__u64 peer_dev_ov_position;
	__u64 peer_dev_ov_left;
	__u64 peer_dev_ov_skipped;
	__u64 peer_dev_rs_same_csum;
	__u64 peer_dev_rs_dt_start_ms;
	__u64 peer_dev_rs_paused_ms;
	__u64 peer_dev_rs_dt0_ms;
	__u64 peer_dev_rs_db0_sectors;
	__u64 peer_dev_rs_dt1_ms;
	__u64 peer_dev_rs_db1_sectors;
	__u32 peer_dev_rs_c_sync_rate;
	__u64 peer_dev_uuid_flags;
};

struct drbd_nl_notification_header {
	__u32 nh_type;
};

struct drbd_nl_helper_info {
	char helper_name[32];
	__u32 helper_name_len;
	__u32 helper_status;
};

struct drbd_invalidate_parms {
	__s32 sync_from_peer_node_id;
	unsigned char reset_bitmap;
};

struct drbd_forget_peer_parms {
	__s32 forget_peer_node_id;
};

struct drbd_peer_device_conf {
	__u32 resync_rate;
	__u32 c_plan_ahead;
	__u32 c_delay_target;
	__u32 c_fill_target;
	__u32 c_max_rate;
	__u32 c_min_rate;
	unsigned char bitmap;
	unsigned char resync_without_replication;
	unsigned char peer_tiebreaker;
};

struct drbd_path_parms {
	char my_addr[128];
	__u32 my_addr_len;
	char peer_addr[128];
	__u32 peer_addr_len;
};

struct drbd_connect_parms {
	unsigned char tentative;
	unsigned char discard_my_data;
};

struct drbd_nl_path_info {
	unsigned char path_established;
};

struct drbd_rename_resource_parms {
	char new_resource_name[128];
	__u32 new_resource_name_len;
};

struct drbd_rename_resource_info {
	char res_new_name[128];
	__u32 res_new_name_len;
};

struct drbd_invalidate_peer_parms {
	unsigned char p_reset_bitmap;
};

struct drbd_suspend_io_parms {
	unsigned char bdev_freeze;
	unsigned char flush_to_peers;
};

struct drbd_cbt_start_parms {
	__u64 cbt_id;
};

struct drbd_cbt_info {
	unsigned char cbt_active;
	unsigned char cbt_valid;
	__u64 cbt_bookmark_id;
	__u64 cbt_since;
	__u64 cbt_bitmap_uuid;
	__u64 cbt_bitmap_dagtag;
};

/*
 * Neutral config defaults, shared by every dialect: default-value setters
 * for the structs above that have optional fields. The core calls these
 * before parsing a partial request, regardless of which dialect received
 * it; the definitions live in linux/drbd_nl_defaults.c, which is always
 * built into the module regardless of dialect configuration.
 */
void drbd_set_nl_cfg_context_defaults(struct drbd_nl_cfg_context *x);
void drbd_set_disk_conf_defaults(struct drbd_disk_conf *x);
void drbd_set_res_opts_defaults(struct drbd_res_opts *x);
void drbd_set_net_conf_defaults(struct drbd_net_conf *x);
void drbd_set_resize_parms_defaults(struct drbd_resize_parms *x);
void drbd_set_detach_parms_defaults(struct drbd_detach_parms *x);
void drbd_set_device_conf_defaults(struct drbd_device_conf *x);
void drbd_set_invalidate_parms_defaults(struct drbd_invalidate_parms *x);
void drbd_set_forget_peer_parms_defaults(struct drbd_forget_peer_parms *x);
void drbd_set_peer_device_conf_defaults(struct drbd_peer_device_conf *x);
void drbd_set_connect_parms_defaults(struct drbd_connect_parms *x);
void drbd_set_invalidate_peer_parms_defaults(struct drbd_invalidate_peer_parms *x);
void drbd_set_suspend_io_parms_defaults(struct drbd_suspend_io_parms *x);

#endif /* __DRBD_NL_TYPES_H */
