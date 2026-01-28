#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright(c) 2026: Mauro Carvalho Chehab <mchehab@kernel.org>.
#
# pylint: disable=C0413,R0904


"""
Unit tests for kernel-doc NestedMatch.
"""

import os
import re
import sys
import unittest

# Import Python modules

SRC_DIR = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, os.path.join(SRC_DIR, "../lib/python"))

from kdoc.kdoc_re import NestedMatch
from unittest_helper import run_unittest

# --------------------------------------------------------------------------
# Verify if struct_group matches are properly handled
# --------------------------------------------------------------------------


class TestStructGroup(unittest.TestCase):
    """
    Test diferent struct_group patterns.

    Please notice that in this class, there are multiple whitespaces on
    some places. That's because it tries to mimic how kernel-doc parser
    internally works.
    """

    @classmethod
    def setUpClass(cls):
        """
        Define a NestedMatch to be used for all tests picking all
        struct_group macros.
        """
        cls.matcher = NestedMatch(r"\bstruct_group[\w\_]*\(")

    def _check_matches(self, line: str, expected_count: int):
        """count and validate each match"""

        matches = list(self.matcher.search(line))
        self.assertEqual(len(matches), expected_count,
                         msg=f"Expected {expected_count} matches, got {len(matches)}")

        for m in matches:
            self.assertTrue(m.startswith("struct_group") and "(" in m,
                            msg=f"Match does not start correctly: {m!r}")
            self.assertTrue(m.endswith(")"),
                            msg=f"Match does not end correctly: {m!r}")

    def test_struct_group_01(self):
        """one struct_group with nested delimiters."""
        line = (
            "__be16 id; struct_group(body, __be16 epl_len; u8 lpl_len; u8"
            " chk_code; u8 resv1; u8 resv2; u8"
            " payload[ETHTOOL_CMIS_CDB_LPL_MAX_PL_LENGTH]; ); u8 *epl;"
        )
        self._check_matches(line, 1)

    def test_struct_group_02(self):
        """two struct_group_tagged, one per page_pool_params."""
        line = (
            "struct_group_tagged(page_pool_params_fast, fast, unsigned int   "
            " order; unsigned int    pool_size; int             nid; struct"
            " device   *dev; struct napi_struct *napi; enum dma_data_direction"
            " dma_dir; unsigned int    max_len; unsigned int    offset; );"
            " struct_group_tagged(page_pool_params_slow, slow, struct"
            " net_device *netdev; unsigned int queue_idx; unsigned int   "
            " flags;)"
        )
        self._check_matches(line, 2)

    def test_struct_group_03(self):
        """two struct_group_tagged, one with nested structs."""
        line = (
            "struct_group_tagged(libeth_xskfq_fp, fp, struct xsk_buff_pool   "
            " *pool; struct libeth_xdp_buff  **fqes; void                   "
            " *descs; u32                     ntu; u32                    "
            " count; );u32                     pending; u32                   "
            "  thresh; u32                     buf_len; int                   "
            "  nid;"
        )
        self._check_matches(line, 1)

    def test_struct_group_04(self):
        """one struct_group_tagged with many fields."""
        line = (
            "struct_group_tagged(libeth_fq_fp, fp, struct page_pool       "
            " *pp; struct libeth_fqe       *fqes; u32                    "
            " truesize; u32                     count; );enum libeth_fqe_type "
            "   type:2; bool                    hsplit:1; bool                "
            "    xdp:1; u32                     buf_len; int                  "
            "   nid;"
        )
        self._check_matches(line, 1)

    def test_struct_group_05(self):
        """long line with a struct_group(priv_flags_fast)."""
        line = (
            "  struct_group(priv_flags_fast, unsigned long          "
            " priv_flags:32; unsigned long           lltx:1; unsigned long    "
            "       netmem_tx:1; ); const struct net_device_ops *netdev_ops;"
            " const struct header_ops *header_ops; struct netdev_queue    "
            " *_tx; netdev_features_t       gso_partial_features; unsigned int"
            "            real_num_tx_queues; unsigned int           "
            " gso_max_size; unsigned int            gso_ipv4_max_size; u16    "
            "                 gso_max_segs; s16                    "
            " num_tc;unsigned int            mtu; unsigned short         "
            " needed_headroom; struct netdev_tc_txq   "
            " tc_to_txq[TC_MAX_QUEUE]; #ifdef CONFIG_XPS; struct xps_dev_maps "
            " *xps_maps[XPS_MAPS_MAX]; #endif; #ifdef CONFIG_NETFILTER_EGRESS;"
            " struct nf_hook_entries  *nf_hooks_egress; #endif; #ifdef"
            " CONFIG_NET_XGRESS; struct bpf_mprog_entry  *tcx_egress; #endif;"
            " union { struct pcpu_lstats __percpu             *lstats; struct"
            " pcpu_sw_netstats __percpu        *tstats; struct pcpu_dstats"
            " __percpu             *dstats; }; unsigned long           state;"
            " unsigned int            flags; unsigned short         "
            " hard_header_len; netdev_features_t       features; struct"
            " inet6_dev   *ip6_ptr; struct bpf_prog    *xdp_prog; struct"
            " list_head        ptype_specific; int                    "
            " ifindex; unsigned int            real_num_rx_queues; struct"
            " netdev_rx_queue  *_rx; unsigned int            gro_max_size;"
            " unsigned int            gro_ipv4_max_size; rx_handler_func_t "
            " *rx_handler; void               *rx_handler_data; possible_net_t"
            "                  nd_net; #ifdef CONFIG_NETPOLL; struct"
            " netpoll_info        *npinfo; #endif; #ifdef CONFIG_NET_XGRESS;"
            " struct bpf_mprog_entry  *tcx_ingress; #endif; char              "
            "      name[IFNAMSIZ]; struct netdev_name_node *name_node; struct"
            " dev_ifalias  *ifalias;unsigned long           mem_end; unsigned"
            " long           mem_start; unsigned long          "
            " base_addr;struct list_head        dev_list; struct list_head    "
            "    napi_list; struct list_head        unreg_list; struct"
            " list_head        close_list; struct list_head        ptype_all;"
            " struct { struct list_head upper; struct list_head lower; }"
            " adj_list;xdp_features_t          xdp_features; const struct"
            " xdp_metadata_ops *xdp_metadata_ops; const struct"
            " xsk_tx_metadata_ops *xsk_tx_metadata_ops; unsigned short        "
            "  gflags; unsigned short          needed_tailroom;"
            " netdev_features_t       hw_features; netdev_features_t      "
            " wanted_features; netdev_features_t       vlan_features;"
            " netdev_features_t       hw_enc_features; netdev_features_t      "
            " mpls_features; unsigned int            min_mtu; unsigned int    "
            "        max_mtu; unsigned short          type; unsigned char     "
            "      min_header_len; unsigned char           name_assign_type;"
            " int                     group; struct net_device_stats"
            " stats;struct net_device_core_stats __percpu *core_stats;atomic_t"
            " tx_request;"
        )
        self._check_matches(line, 1)

    def test_struct_group_06(self):
        """struct_group(addrs)."""
        line = (
            "struct_group(addrs, unsigned char   h_dest[ETH_ALEN]; unsigned"
            " char   h_source[ETH_ALEN]; ); __be16          h_vlan_proto;"
            " __be16          h_vlan_TCI; __be16         "
            " h_vlan_encapsulated_proto;"
        )
        self._check_matches(line, 1)

    def test_struct_group_07(self):
        """one struct_group(headers)."""
        line = (
            "union { struct {struct sk_buff          *next; struct sk_buff    "
            "      *prev; union { struct net_device       *dev;unsigned long  "
            "         dev_scratch; }; }; struct rb_node          rbnode;struct"
            " list_head        list; struct llist_node       ll_node; };"
            " struct sock             *sk; union { ktime_t         tstamp; u64"
            "             skb_mstamp_ns;};char                    cb[48] ;"
            " union { struct { unsigned long   _skb_refdst; void           "
            " (*destructor)(struct sk_buff *skb); }; struct list_head       "
            " tcp_tsorted_anchor; #ifdef CONFIG_NET_SOCK_MSG; unsigned long   "
            "        _sk_redir; #endif; }; #if defined(CONFIG_NF_CONNTRACK) ||"
            " defined(CONFIG_NF_CONNTRACK_MODULE); unsigned long           "
            " _nfct; #endif; unsigned int            len, data_len; __u16     "
            "              mac_len, hdr_len;__u16                  "
            " queue_mapping;#ifdef __BIG_ENDIAN_BITFIELD; #define CLONED_MASK "
            "    (1 << 7); #else; #define CLONED_MASK     1; #endif; #define"
            " CLONED_OFFSET           offsetof(struct sk_buff,"
            " __cloned_offset);  __u8                    cloned:1, nohdr:1,"
            " fclone:2, peeked:1, head_frag:1, pfmemalloc:1,"
            " pp_recycle:1;#ifdef CONFIG_SKB_EXTENSIONS; __u8                 "
            "   active_extensions; #endif;struct_group(headers,  __u8         "
            "           pkt_type:3;__u8                    ignore_df:1; __u8  "
            "                  dst_pending_confirm:1; __u8                   "
            " ip_summed:2; __u8                    ooo_okay:1;  __u8          "
            "          tstamp_type:2;#ifdef CONFIG_NET_XGRESS; __u8           "
            "         tc_at_ingress:1;__u8                   "
            " tc_skip_classify:1; #endif; __u8                   "
            " remcsum_offload:1; __u8                    csum_complete_sw:1;"
            " __u8                    csum_level:2; __u8                   "
            " inner_protocol_type:1; __u8                    l4_hash:1; __u8  "
            "                  sw_hash:1; #ifdef CONFIG_WIRELESS; __u8        "
            "            wifi_acked_valid:1; __u8                   "
            " wifi_acked:1; #endif; __u8                    no_fcs:1;__u8     "
            "               encapsulation:1; __u8                   "
            " encap_hdr_csum:1; __u8                    csum_valid:1; #ifdef"
            " CONFIG_IPV6_NDISC_NODETYPE; __u8                   "
            " ndisc_nodetype:2; #endif; #if IS_ENABLED(CONFIG_IP_VS); __u8    "
            "                ipvs_property:1; #endif; #if"
            " IS_ENABLED(CONFIG_NETFILTER_XT_TARGET_TRACE) ||"
            " IS_ENABLED(CONFIG_NF_TABLES); __u8                   "
            " nf_trace:1; #endif; #ifdef CONFIG_NET_SWITCHDEV; __u8           "
            "         offload_fwd_mark:1; __u8                   "
            " offload_l3_fwd_mark:1; #endif; __u8                   "
            " redirected:1; #ifdef CONFIG_NET_REDIRECT; __u8                  "
            "  from_ingress:1; #endif; #ifdef CONFIG_NETFILTER_SKIP_EGRESS;"
            " __u8                    nf_skip_egress:1; #endif; #ifdef"
            " CONFIG_SKB_DECRYPTED; __u8                    decrypted:1;"
            " #endif; __u8                    slow_gro:1; #if"
            " IS_ENABLED(CONFIG_IP_SCTP); __u8                   "
            " csum_not_inet:1; #endif; __u8                    unreadable:1;"
            " #if defined(CONFIG_NET_SCHED) || defined(CONFIG_NET_XGRESS);"
            " __u16                   tc_index;#endif; u16                    "
            " alloc_cpu; union { __wsum          csum; struct { __u16  "
            " csum_start; __u16   csum_offset; }; }; __u32                  "
            " priority; int                     skb_iif; __u32                "
            "   hash; union { u32             vlan_all; struct { __be16 "
            " vlan_proto; __u16   vlan_tci; }; }; #if"
            " defined(CONFIG_NET_RX_BUSY_POLL) || defined(CONFIG_XPS); union {"
            " unsigned int    napi_id; unsigned int    sender_cpu; }; };"
            " #ifdef CONFIG_NETWORK_SECMARK; __u32           secmark; #endif;"
            " union { __u32           mark; __u32           reserved_tailroom;"
            " }; union { __be16          inner_protocol; __u8           "
            " inner_ipproto; }; __u16                  "
            " inner_transport_header; __u16                  "
            " inner_network_header; __u16                   inner_mac_header;"
            " __be16                  protocol; __u16                  "
            " transport_header; __u16                   network_header; __u16 "
            "                  mac_header; #ifdef CONFIG_KCOV; u64            "
            "         kcov_handle; #endif; );sk_buff_data_t          tail;"
            " sk_buff_data_t          end; unsigned char           *head,"
            " *data; unsigned int            truesize; refcount_t             "
            " users; #ifdef CONFIG_SKB_EXTENSIONS;struct skb_ext         "
            " *extensions; #endif;"
        )
        self._check_matches(line, 1)

    def test_struct_group_08(self):
        """two struct_group(stats)."""
        line = (
            "enum ethtool_mac_stats_src src; struct_group(stats, u64"
            " tx_pause_frames; u64 rx_pause_frames; ); enum"
            " ethtool_mac_stats_src src; struct_group(stats, u64"
            " undersize_pkts; u64 oversize_pkts; u64 fragments; u64 jabbers;"
            " u64 hist[ETHTOOL_RMON_HIST_MAX]; u64"
            " hist_tx[ETHTOOL_RMON_HIST_MAX]; );"
        )
        self._check_matches(line, 2)

    def test_struct_group_09(self):
        """struct_group(tx_stats)."""
        line = (
            "struct_group(tx_stats, u64 pkts; u64 onestep_pkts_unconfirmed;"
            " u64 lost; u64 err; );"
        )
        self._check_matches(line, 1)

    def test_struct_group_10(self):
        """struct_group(zeroed_on_hw_restart) with a nested struct."""
        line = (
            "struct_group(zeroed_on_hw_restart, u16 fw_id; struct { u8"
            " allocated:1; u8 stop_full:1; } status; ); struct list_head list;"
            " atomic_t tx_request;"
        )
        self._check_matches(line, 1)

    def test_struct_group_11(self):
        """struct_group(zeroed_on_hw_restart) with many fields."""
        line = (
            "struct_group(zeroed_on_hw_restart, unsigned int status; u32"
            " uid_status[IWL_MAX_UMAC_SCANS]; u64 start_tsf; bool"
            " last_ebs_failed; enum iwl_mld_pass_all_sched_results_states"
            " pass_all_sched_res; u8 fw_link_id; struct { u32"
            " last_stats_ts_usec; enum iwl_mld_traffic_load status; }"
            " traffic_load; );size_t cmd_size; void *cmd; unsigned long"
            " last_6ghz_passive_jiffies; unsigned long"
            " last_start_time_jiffies; u64 last_mlo_scan_time;"
        )
        self._check_matches(line, 1)

    def test_struct_group_12(self):
        """struct_group(zeroed_on_hw_restart) with a huge struct."""
        line = (
            "struct_group(zeroed_on_hw_restart, struct ieee80211_bss_conf "
            " *fw_id_to_bss_conf[IWL_FW_MAX_LINK_ID + 1]; struct ieee80211_vif"
            "  *fw_id_to_vif[NUM_MAC_INDEX_DRIVER]; struct ieee80211_txq "
            " *fw_id_to_txq[IWL_MAX_TVQM_QUEUES]; u8 used_phy_ids:"
            " NUM_PHY_CTX; u8 num_igtks; struct { bool on; u32 ampdu_ref; bool"
            " ampdu_toggle; u8 p80; struct { struct"
            " iwl_rx_phy_air_sniffer_ntfy data; u8 valid:1, used:1; } phy;"
            " #ifdef CONFIG_IWLWIFI_DEBUGFS; __le16 cur_aid; u8"
            " cur_bssid[ETH_ALEN]; bool ptp_time; #endif; } monitor; #ifdef"
            " CONFIG_PM_SLEEP; bool netdetect; #endif; struct ieee80211_vif"
            " *p2p_device_vif; bool bt_is_active; struct ieee80211_vif"
            " *nan_device_vif; ); struct ieee80211_link_sta "
            " *fw_id_to_link_sta[IWL_STATION_COUNT_MAX];struct device *dev;"
            " struct iwl_trans *trans; const struct iwl_rf_cfg *cfg; const"
            " struct iwl_fw *fw; struct ieee80211_hw *hw; struct wiphy *wiphy;"
            " struct wiphy_iftype_ext_capab"
            " ext_capab[IWL_MLD_EXT_CAPA_NUM_IFTYPES]; u8"
            " sta_ext_capab[IWL_MLD_STA_EXT_CAPA_SIZE]; struct iwl_nvm_data"
            " *nvm_data; struct iwl_fw_runtime fwrt; struct dentry"
            " *debugfs_dir; struct iwl_notif_wait_data notif_wait; struct"
            " list_head async_handlers_list; spinlock_t async_handlers_lock;"
            " struct wiphy_work async_handlers_wk; struct wiphy_delayed_work"
            " ct_kill_exit_wk; struct { u32 running:1, do_not_dump_once:1,"
            " #ifdef CONFIG_PM_SLEEP; in_d3:1, resuming:1, #endif;"
            " in_hw_restart:1; } fw_status; struct { u32 hw:1, ct:1; }"
            " radio_kill; u32 power_budget_mw; struct mac_address"
            " addresses[IWL_MLD_MAX_ADDRESSES]; struct iwl_mld_scan scan;"
            " struct iwl_mld_survey *channel_survey; #ifdef CONFIG_PM_SLEEP;"
            " struct wiphy_wowlan_support wowlan; u32 debug_max_sleep; #endif;"
            " #ifdef CONFIG_IWLWIFI_LEDS; struct led_classdev led; #endif;"
            " enum iwl_mcc_source mcc_src; bool bios_enable_puncturing; struct"
            " iwl_mld_baid_data  *fw_id_to_ba[IWL_MAX_BAID]; u8"
            " num_rx_ba_sessions; struct iwl_mld_rx_queues_sync rxq_sync;"
            " struct list_head txqs_to_add; struct wiphy_work add_txqs_wk;"
            " spinlock_t add_txqs_lock; u8 *error_recovery_buf; struct"
            " iwl_mcast_filter_cmd *mcast_filter_cmd; u8 mgmt_tx_ant; u8"
            " set_tx_ant; u8 set_rx_ant; bool fw_rates_ver_3; struct"
            " iwl_mld_low_latency low_latency; bool ibss_manager; #ifdef"
            " CONFIG_THERMAL; struct thermal_zone_device *tzone; struct"
            " iwl_mld_cooling_device cooling_dev; #endif; struct ptp_data"
            " ptp_data; struct iwl_mld_time_sync_data  *time_sync; struct"
            " ftm_initiator_data ftm_initiator;"
        )
        self._check_matches(line, 1)

    def test_struct_group_13(self):
        """struct_group(zeroed_on_not_authorized)."""
        line = (
            "struct_group(zeroed_on_not_authorized, u8 primary; u8"
            " selected_primary; u16 selected_links; enum iwl_mld_emlsr_blocked"
            " blocked_reasons; enum iwl_mld_emlsr_exit last_exit_reason;"
            " unsigned long last_exit_ts; u8 exit_repeat_count; unsigned long"
            " last_entry_ts; ); struct wiphy_work unblock_tpt_wk; struct"
            " wiphy_delayed_work check_tpt_wk; struct wiphy_delayed_work"
            " prevent_done_wk; struct wiphy_delayed_work tmp_non_bss_done_wk;"
        )
        self._check_matches(line, 1)

    def test_struct_group_14(self):
        """struct_group(zeroed_on_hw_restart) with nested struct."""
        line = (
            "struct_group(zeroed_on_hw_restart, u8 fw_id; struct"
            " iwl_mld_session_protect session_protect; struct ieee80211_sta"
            " *ap_sta; bool authorized; u8 num_associated_stas; bool"
            " ap_ibss_active; enum iwl_mld_cca_40mhz_wa_status"
            " cca_40mhz_workaround; #ifdef CONFIG_IWLWIFI_DEBUGFS; bool"
            " beacon_inject_active; #endif; u8 low_latency_causes; bool"
            " ps_disabled; time64_t last_link_activation_time; );struct"
            " iwl_mld *mld; struct iwl_mld_link deflink; struct iwl_mld_link "
            " *link[IEEE80211_MLD_MAX_NUM_LINKS]; struct iwl_mld_emlsr emlsr;"
            " #ifdef CONFIG_PM_SLEEP; struct iwl_mld_wowlan_data wowlan_data;"
            " #endif; #ifdef CONFIG_IWLWIFI_DEBUGFS; bool use_ps_poll; bool"
            " disable_bf; struct dentry *dbgfs_slink; #endif; enum"
            " iwl_roc_activity roc_activity; struct iwl_mld_int_sta aux_sta;"
            " struct wiphy_delayed_work mlo_scan_start_wk;"
        )
        self._check_matches(line, 1)

    def test_struct_group_15(self):
        """struct_group(zeroed_on_hw_restart) with small struct."""
        line = (
            "struct_group(zeroed_on_hw_restart, u32 last_rate_n_flags; bool"
            " in_fw; s8 signal_avg; );struct rcu_head rcu_head; u32 fw_id;"
        )
        self._check_matches(line, 1)

    def test_struct_group_16(self):
        """struct_group(zeroed_on_hw_restart) with many enums."""
        line = (
            "struct_group(zeroed_on_hw_restart, enum ieee80211_sta_state"
            " sta_state; enum iwl_fw_sta_type sta_type; );struct iwl_mld *mld;"
            " struct ieee80211_vif *vif; struct iwl_mld_rxq_dup_data"
            " *dup_data; u8 tid_to_baid[IWL_MAX_TID_COUNT]; u8 data_tx_ant;"
            " struct iwl_mld_link_sta deflink; struct iwl_mld_link_sta "
            " *link[IEEE80211_MLD_MAX_NUM_LINKS]; struct iwl_mld_ptk_pn "
            " *ptk_pn[IWL_NUM_DEFAULT_KEYS]; struct iwl_mld_per_q_mpdu_counter"
            " *mpdu_counters;"
        )
        self._check_matches(line, 1)

    def test_struct_group_17(self):
        """struct_group(zeroed_on_hw_restart) with channel data."""
        line = (
            "struct_group(zeroed_on_hw_restart, u8 fw_id; struct"
            " cfg80211_chan_def chandef; );u32 channel_load_by_us; u32"
            " avg_channel_load_not_by_us; struct iwl_mld *mld;"
        )
        self._check_matches(line, 1)

    def test_struct_group_18(self):
        """mixture of struct_group and struct rcu_head."""
        line = (
            "struct rcu_head rcu_head;struct_group(zeroed_on_hw_restart, u8"
            " fw_id; bool active; struct ieee80211_tx_queue_params"
            " queue_params[IEEE80211_NUM_ACS]; struct ieee80211_chanctx_conf "
            " *chan_ctx; bool he_ru_2mhz_block; struct ieee80211_key_conf"
            " *igtk; struct ieee80211_key_conf  *bigtks[2]; );struct"
            " iwl_mld_int_sta bcast_sta; struct iwl_mld_int_sta mcast_sta;"
            " struct iwl_mld_int_sta mon_sta;struct ieee80211_key_conf"
            " *ap_early_keys[6]; u32 average_beacon_energy; bool"
            " silent_deactivation; struct iwl_probe_resp_data "
            " *probe_resp_data;"
        )
        self._check_matches(line, 1)

    def test_struct_group_19(self):
        """x(ice_health_tx_hang_buf)."""
        line = (
            "struct devlink_health_reporter *fw; struct"
            " devlink_health_reporter *mdd; struct devlink_health_reporter"
            " *port; struct devlink_health_reporter *tx_hang;"
            " struct_group_tagged(ice_health_tx_hang_buf, tx_hang_buf, struct"
            " ice_tx_ring *tx_ring; u32 head; u32 intr; u16 vsi_num; ); struct"
            " ice_aqc_health_status_elem fw_status; struct"
            " ice_aqc_health_status_elem port_status;"
        )
        self._check_matches(line, 1)

    def test_struct_group_sub(self):
        """Replace struct_group body with a placeholder."""
        line = "foo bar struct_group(my, a(b{c}), d); qux;"

        result = NestedMatch(r"\bstruct_group\(").sub("REPLACED", line)
        expected = "foo bar REPLACED qux;"

        self.assertEqual(result, expected)


class TestSubMacros(unittest.TestCase):
    """
    Test macros that will be dropped.
    """

    def test_acquires_simple(self):
        """Simple replacement test with __acquires"""
        line = "__acquires(ctx) foo();"
        result = NestedMatch(r"__acquires\s*\(").sub("REPLACED", line)

        self.assertNotIn("__acquires(", result)
        self.assertIn("foo();", result)

    def test_acquires_multiple(self):
        """Multiple __acquires"""
        line = "__acquires(ctx) __acquires(other) bar();"
        result = NestedMatch(r"__acquires\s*\(").sub("REPLACED", line)

        self.assertNotIn("__acquires(", result)
        self.assertEqual(result.count("REPLACED"), 2)

    def test_acquires_nested_paren(self):
        """__acquires with nested pattern"""
        line = "__acquires((ctx1, ctx2)) baz();"
        result = NestedMatch(r"__acquires\s*\(").sub("REPLACED", line)

        self.assertNotIn("__acquires(", result)
        self.assertIn("baz();", result)

    def test_must_hold(self):
        """__must_hold with a pointer"""
        line = "__must_hold(&lock) do_something();"
        result = NestedMatch(r"__must_hold\s*\(").sub("REPLACED", line)

        self.assertNotIn("__must_hold(", result)
        self.assertIn("do_something();", result)

    def test_must_hold_shared(self):
        """__must_hold with an upercase defined value"""
        line = "__must_hold_shared(RCU) other();"
        result = NestedMatch(r"__must_hold_shared\s*\(").sub("REPLACED", line)

        self.assertNotIn("__must_hold_shared(", result)
        self.assertIn("other();", result)

    def test_no_false_positive(self):
        """
        Ensure that unrelated text containing similar patterns is preserved
        """
        line = "call__acquires(foo);  // should stay intact"
        result = NestedMatch(r"\b__acquires\s*\(").sub("REPLACED", line)

        self.assertEqual(result, line)

    def test_mixed_macros(self):
        """Add a mix of macros"""
        line = "__acquires(ctx) __releases(ctx) __must_hold(&lock) foo();"

        result = NestedMatch(r"__acquires\s*\(").sub("REPLACED", line)
        result = NestedMatch(r"__releases\s*\(").sub("REPLACED", result)
        result = NestedMatch(r"__must_hold\s*\(").sub("REPLACED", result)

        self.assertNotIn("__acquires(", result)
        self.assertNotIn("__releases(", result)
        self.assertNotIn("__must_hold(", result)

        self.assertIn("foo();", result)

    def test_no_macro_remains(self):
        """Ensures that unmatched macros are untouched"""
        line = "do_something_else();"
        result = NestedMatch(r"__acquires\s*\(").sub("REPLACED", line)

        self.assertEqual(result, line)


class TestSubReplacement(unittest.TestCase):
    """Test argument replacements"""

    @classmethod
    def setUpClass(cls):
        """Define a NestedMatch to be used for all tests"""
        cls.matcher = NestedMatch(re.compile(r"__acquires\s*\("))

    def test_sub_with_capture(self):
        """Test all arguments replacement with a single arg"""
        line = "__acquires(&ctx) foo();"

        result = self.matcher.sub(r"ACQUIRED(\0)", line)

        self.assertIn("ACQUIRED(&ctx)", result)
        self.assertIn("foo();", result)

    def test_sub_zero_placeholder(self):
        """Test all arguments replacement with a multiple args"""
        line = "__acquires(arg1, arg2) bar();"

        result = self.matcher.sub(r"REPLACED(\0)", line)

        self.assertIn("bar();", result)
        self.assertIn("REPLACED(arg1, arg2)", result)

    def test_sub_single_placeholder(self):
        """Single replacement rule for \1"""
        line = "__acquires(ctx) foo();"
        result = self.matcher.sub(r"ACQUIRED(\1)", line)

        self.assertIn("foo();", result)
        self.assertIn("ACQUIRED(ctx)", result)

    def test_sub_multiple_placeholders(self):
        """Replacement rule for both \1 and \2"""
        line = "__acquires(arg1, arg2) bar();"
        result = self.matcher.sub(r"REPLACE(\1, \2)", line)

        self.assertIn("bar();", result)
        self.assertIn("REPLACE(arg1, arg2)", result)

    def test_sub_mixed_placeholders(self):
        """Replacement rule for \0, \1 and additional text"""
        line = "__acquires(foo, bar) baz();"
        result = self.matcher.sub(r"START(\0) END(\1)", line)

        self.assertIn("baz();", result)
        self.assertIn("START(foo, bar) END(foo)", result)

    def test_sub_no_placeholder(self):
        """Replacement without placeholders"""
        line = "__acquires(arg) foo();"
        result = self.matcher.sub(r"NO_BACKREFS()", line)

        self.assertIn("foo();", result)
        self.assertIn("NO_BACKREFS()", result)

    def test_sub_count_parameter(self):
        """Verify that the algorithm stops after the requested count"""
        line = "__acquires(a1) x(); __acquires(a2) y();"
        result = self.matcher.sub(r"ONLY_FIRST(\1)", line, count=1)

        self.assertIn("ONLY_FIRST(a1) x();", result)
        self.assertIn("__acquires(a2) y();", result)


# ----------------------------------------------------------------------
# Run all tests
# ----------------------------------------------------------------------
if __name__ == "__main__":
    run_unittest(__file__)
