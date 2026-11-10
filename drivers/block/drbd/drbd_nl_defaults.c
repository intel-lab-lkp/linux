// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)
/*
 * Default setters for the structs in drbd_nl_types.h, generated from
 * drbd_genl_ynl.yaml by the YNL generator carried with the out-of-tree
 * DRBD sources (drbd-headers, linux/generate.sh). The kernel's
 * tools/net/ynl cannot regenerate this file.
 */

#include "drbd_nl_types.h"
#include <linux/string.h>

#include <linux/drbd.h>
#include <linux/drbd_limits.h>

void drbd_set_nl_cfg_context_defaults(struct drbd_nl_cfg_context *x)
{
	memset(x->ctx_conn_name, 0, sizeof(x->ctx_conn_name));
	x->ctx_conn_name_len = 0;
}

void drbd_set_disk_conf_defaults(struct drbd_disk_conf *x)
{
	x->on_io_error = DRBD_ON_IO_ERROR_DEF;
	x->resync_after = DRBD_MINOR_NUMBER_DEF;
	x->al_extents = DRBD_AL_EXTENTS_DEF;
	x->disk_barrier = DRBD_DISK_BARRIER_DEF;
	x->disk_flushes = DRBD_DISK_FLUSHES_DEF;
	x->disk_drain = DRBD_DISK_DRAIN_DEF;
	x->md_flushes = DRBD_MD_FLUSHES_DEF;
	x->disk_timeout = DRBD_DISK_TIMEOUT_DEF;
	x->read_balancing = DRBD_READ_BALANCING_DEF;
	x->unplug_watermark = DRBD_UNPLUG_WATERMARK_DEF;
	x->al_updates = DRBD_AL_UPDATES_DEF;
	x->discard_zeroes_if_aligned = DRBD_DISCARD_ZEROES_IF_ALIGNED_DEF;
	x->rs_discard_granularity = DRBD_RS_DISCARD_GRANULARITY_DEF;
	x->disable_write_same = DRBD_DISABLE_WRITE_SAME_DEF;
	x->d_bitmap = DRBD_BITMAP_DEF;
}

void drbd_set_res_opts_defaults(struct drbd_res_opts *x)
{
	memset(x->cpu_mask, 0, sizeof(x->cpu_mask));
	x->cpu_mask_len = 0;
	x->on_no_data = DRBD_ON_NO_DATA_DEF;
	x->auto_promote = DRBD_AUTO_PROMOTE_DEF;
	x->peer_ack_window = DRBD_PEER_ACK_WINDOW_DEF;
	x->twopc_timeout = DRBD_TWOPC_TIMEOUT_DEF;
	x->twopc_retry_timeout = DRBD_TWOPC_RETRY_TIMEOUT_DEF;
	x->peer_ack_delay = DRBD_PEER_ACK_DELAY_DEF;
	x->auto_promote_timeout = DRBD_AUTO_PROMOTE_TIMEOUT_DEF;
	x->nr_requests = DRBD_NR_REQUESTS_DEF;
	x->quorum = DRBD_QUORUM_DEF;
	x->on_no_quorum = DRBD_ON_NO_QUORUM_DEF;
	x->quorum_min_redundancy = DRBD_QUORUM_DEF;
	x->on_susp_primary_outdated = DRBD_ON_SUSP_PRI_OUTD_DEF;
	x->drbd8_compat_mode = DRBD_DRBD8_COMPAT_MODE_DEF;
	x->explicit_drbd8_compat = DRBD_DRBD8_COMPAT_MODE_DEF;
}

void drbd_set_net_conf_defaults(struct drbd_net_conf *x)
{
	memset(x->shared_secret, 0, sizeof(x->shared_secret));
	x->shared_secret_len = 0;
	memset(x->cram_hmac_alg, 0, sizeof(x->cram_hmac_alg));
	x->cram_hmac_alg_len = 0;
	memset(x->integrity_alg, 0, sizeof(x->integrity_alg));
	x->integrity_alg_len = 0;
	memset(x->verify_alg, 0, sizeof(x->verify_alg));
	x->verify_alg_len = 0;
	memset(x->csums_alg, 0, sizeof(x->csums_alg));
	x->csums_alg_len = 0;
	x->wire_protocol = DRBD_PROTOCOL_DEF;
	x->connect_int = DRBD_CONNECT_INT_DEF;
	x->timeout = DRBD_TIMEOUT_DEF;
	x->ping_int = DRBD_PING_INT_DEF;
	x->ping_timeo = DRBD_PING_TIMEO_DEF;
	x->sndbuf_size = DRBD_SNDBUF_SIZE_DEF;
	x->rcvbuf_size = DRBD_RCVBUF_SIZE_DEF;
	x->ko_count = DRBD_KO_COUNT_DEF;
	x->max_epoch_size = DRBD_MAX_EPOCH_SIZE_DEF;
	x->after_sb_0p = DRBD_AFTER_SB_0P_DEF;
	x->after_sb_1p = DRBD_AFTER_SB_1P_DEF;
	x->after_sb_2p = DRBD_AFTER_SB_2P_DEF;
	x->rr_conflict = DRBD_RR_CONFLICT_DEF;
	x->on_congestion = DRBD_ON_CONGESTION_DEF;
	x->cong_fill = DRBD_CONG_FILL_DEF;
	x->cong_extents = DRBD_CONG_EXTENTS_DEF;
	x->two_primaries = DRBD_ALLOW_TWO_PRIMARIES_DEF;
	x->tcp_cork = DRBD_TCP_CORK_DEF;
	x->always_asbp = DRBD_ALWAYS_ASBP_DEF;
	x->use_rle = DRBD_USE_RLE_DEF;
	x->fencing_policy = DRBD_FENCING_DEF;
	memset(x->name, 0, sizeof(x->name));
	x->name_len = 0;
	x->csums_after_crash_only = DRBD_CSUMS_AFTER_CRASH_ONLY_DEF;
	x->sock_check_timeo = DRBD_SOCKET_CHECK_TIMEO_DEF;
	memset(x->transport_name, 0, sizeof(x->transport_name));
	x->transport_name_len = 0;
	x->max_buffers = DRBD_MAX_BUFFERS_DEF;
	x->allow_remote_read = DRBD_ALLOW_REMOTE_READ_DEF;
	x->tls = DRBD_TLS_DEF;
	x->tls_privkey = DRBD_TLS_PRIVKEY_DEF;
	x->tls_certificate = DRBD_TLS_CERTIFICATE_DEF;
	x->tls_keyring = DRBD_TLS_KEYRING_DEF;
	x->load_balance_paths = DRBD_LOAD_BALANCE_PATHS_DEF;
	x->rdma_ctrl_rcvbuf_size = DRBD_RDMA_CTRL_RCVBUF_SIZE_DEF;
	x->rdma_ctrl_sndbuf_size = DRBD_RDMA_CTRL_SNDBUF_SIZE_DEF;
}

void drbd_set_resize_parms_defaults(struct drbd_resize_parms *x)
{
	x->al_stripes = DRBD_AL_STRIPES_DEF;
	x->al_stripe_size = DRBD_AL_STRIPE_SIZE_DEF;
}

void drbd_set_detach_parms_defaults(struct drbd_detach_parms *x)
{
	x->intentional_diskless_detach = DRBD_DISK_DISKLESS_DEF;
}

void drbd_set_device_conf_defaults(struct drbd_device_conf *x)
{
	x->max_bio_size = DRBD_MAX_BIO_SIZE_DEF;
	x->intentional_diskless = DRBD_DISK_DISKLESS_DEF;
	x->block_size = DRBD_BLOCK_SIZE_DEF;
	x->discard_granularity = DRBD_DISCARD_GRANULARITY_DEF;
}

void drbd_set_invalidate_parms_defaults(struct drbd_invalidate_parms *x)
{
	x->sync_from_peer_node_id = DRBD_SYNC_FROM_NID_DEF;
	x->reset_bitmap = DRBD_INVALIDATE_RESET_BITMAP_DEF;
}

void drbd_set_forget_peer_parms_defaults(struct drbd_forget_peer_parms *x)
{
	x->forget_peer_node_id = DRBD_SYNC_FROM_NID_DEF;
}

void drbd_set_peer_device_conf_defaults(struct drbd_peer_device_conf *x)
{
	x->resync_rate = DRBD_RESYNC_RATE_DEF;
	x->c_plan_ahead = DRBD_C_PLAN_AHEAD_DEF;
	x->c_delay_target = DRBD_C_DELAY_TARGET_DEF;
	x->c_fill_target = DRBD_C_FILL_TARGET_DEF;
	x->c_max_rate = DRBD_C_MAX_RATE_DEF;
	x->c_min_rate = DRBD_C_MIN_RATE_DEF;
	x->bitmap = DRBD_BITMAP_DEF;
	x->resync_without_replication = DRBD_RESYNC_WITHOUT_REPLICATION_DEF;
	x->peer_tiebreaker = DRBD_PEER_TIEBREAKER_DEF;
}

void drbd_set_connect_parms_defaults(struct drbd_connect_parms *x)
{
	x->tentative = 0;
	x->discard_my_data = 0;
}

void drbd_set_invalidate_peer_parms_defaults(struct drbd_invalidate_peer_parms *x)
{
	x->p_reset_bitmap = DRBD_INVALIDATE_RESET_BITMAP_DEF;
}

void drbd_set_suspend_io_parms_defaults(struct drbd_suspend_io_parms *x)
{
	x->bdev_freeze = DRBD_SUSPEND_IO_BDEV_FREEZE_DEF;
	x->flush_to_peers = DRBD_SUSPEND_IO_FLUSH_TO_PEERS_DEF;
}
