/* SPDX-License-Identifier: GPL-2.0 */
/*
 * J1939 test-visible functions
 *
 * This header exposes internal functions for KUnit testing.
 *
 * Safe to include unconditionally - empty when CONFIG_KUNIT=n.
 * Functions are static (via VISIBLE_IF_KUNIT) when testing disabled.
 */

#ifndef _J1939_TEST_H_
#define _J1939_TEST_H_

#if IS_ENABLED(CONFIG_KUNIT)

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/can/j1939.h>
#include <linux/can.h>
#include <linux/can/core.h>

/* From socket.c - exposed for KUnit testing via wrapper functions
 * The actual implementations are static inline for performance; these wrappers
 * allow testing without impacting production code.
 */

priority_t j1939_prio_wrapper(u32 sk_priority);
u32 j1939_to_sk_priority_wrapper(priority_t prio);
bool j1939_pgn_is_valid_wrapper(pgn_t pgn);
bool j1939_pgn_is_clean_pdu_wrapper(pgn_t pgn);
int j1939_sk_sanity_check(struct sockaddr_can *addr, int len);

struct j1939_sock;
void j1939_sk_sock2sockaddr_can(struct sockaddr_can *addr,
				const struct j1939_sock *jsk, int peer);

enum j1939_sk_errqueue_type;
size_t j1939_sk_opt_stats_get_size(enum j1939_sk_errqueue_type type);

/* From transport.c - exposed for KUnit testing
 * Functions with _wrapper suffix are non-inline wrappers around static inline
 * implementations to avoid performance impact on production code.
 */

/* Forward declarations for structures (full definitions in j1939-priv.h) */
struct j1939_priv;
struct j1939_sk_buff_cb;
struct j1939_addr;

/* enum j1939_xtp_abort is defined in j1939-priv.h */

/* Non-inline functions exported directly */
const char *j1939_xtp_abort_to_str(enum j1939_xtp_abort abort);
int j1939_xtp_abort_to_errno(struct j1939_priv *priv,
			      enum j1939_xtp_abort abort);
bool j1939_session_match(struct j1939_addr *se_addr,
			 struct j1939_addr *sk_addr, bool reverse);
void j1939_skbcb_swap(struct j1939_sk_buff_cb *skcb);

/* Wrappers for hot-path inline functions */
bool j1939_cb_is_broadcast_wrapper(const struct j1939_sk_buff_cb *skcb);
pgn_t j1939_xtp_ctl_to_pgn_wrapper(const u8 *dat);
unsigned int j1939_tp_ctl_to_size_wrapper(const u8 *dat);
unsigned int j1939_etp_ctl_to_packet_wrapper(const u8 *dat);
unsigned int j1939_etp_ctl_to_size_wrapper(const u8 *dat);

#endif /* IS_ENABLED(CONFIG_KUNIT) */

#endif /* _J1939_TEST_H_ */
