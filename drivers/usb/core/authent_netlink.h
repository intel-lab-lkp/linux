/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SPDX-FileCopyrightText: (C) 2025 ANSSI
 *
 * USB Authentication netlink interface
 *
 * Author: Luc Bonnafoux <luc.bonnafoux@ssi.gouv.fr>
 * Author: Nicolas Bouchinet <nicolas.bouchinet@ssi.gouv.fr>
 *
 */

#ifndef __USB_CORE_AUTHENT_NETLINK_H_
#define __USB_CORE_AUTHENT_NETLINK_H_

int usb_auth_init_netlink(void);
int usb_policy_engine_check_digest(const u32 route,
				   const u8 *const digests,
				   const u8 mask, u8 *is_known,
				   u8 *is_blocked, u32 *id);
int usb_policy_engine_check_cert_chain(const u32 route,
				       const u8 *const digest,
				       const u8 *const chain,
				       const size_t chain_len,
				       u8 *is_valid, u8 *is_blocked,
				       u32 *id);
int usb_policy_engine_generate_challenge(const u32 id, u8 *nonce);
int usb_policy_engine_check_challenge(const u32 id,
				      const u8 *const challenge,
				      const u8 *const context,
				      const size_t context_size,
				      u8 *is_valid);

#endif /* __USB_CORE_AUTHENT_NETLINK_H_ */
