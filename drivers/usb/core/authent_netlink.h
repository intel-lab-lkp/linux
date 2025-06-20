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

/**
 * @brief Check if a digest match a device
 *
 * This function blocks until a response has been received from userspace or in
 * case of timeout.
 * The function blocks if no policy engine is registered with a timeout.
 *
 * Context: task context, might sleep.
 *
 * Possible errors:
 *  - EINVAL   : if digest is NULL
 *  - ECOMM    : if no userspace policy engine is available
 *                  or already userspace is busy
 *                  or message transmission failed
 *  - ENOMEM   : if message creation failed
 *  - EMSGSIZE : if message creation failed
 *
 * @param [in] digest      : USB Authentication digest, must be 256 B
 * @param [in] mask        : USB Authentication slot mask
 * @param [out] is_known   : 1 at each index with a known digest, 0 otherwise
 * @param [out] is_blocked : 1 if the device is known and banned, 0 otherwise
 * @param [out] id         : if is_known and !is_blocked then contains the device handle
 *
 * @return 0 on SUCCESS else error code
 */
int usb_policy_engine_check_digest(const uint32_t route,
				   const uint8_t *const digests,
				   const uint8_t mask, uint8_t *is_known,
				   uint8_t *is_blocked, uint32_t *id);

/**
 * @brief Check if a certificate chain is valid and authorized
 *
 * A certificate chain is valid if it can be successfully verified with one of the
 *  root CA in store.
 * A certificate chain is blocked if one of the certificate of chain is blocked,
 *  due to revocation, blacklist...
 *
 * Context: task context, might sleep.
 *
 * Possible errors:
 *  - EINVAL   : if digest is NULL
 *  - ECOMM    : if no userspace policy engine is available
 *                  or already userspace is busy
 *                  or message transmission failed
 *  - ENOMEM   : if message creation failed
 *  - EMSGSIZE : if message creation failed
 *
 * TODO: see if it is necessary to have a separate boolean for is_blocked
 *
 * @param [in] route        : Information on the device to construct the ID
 * @param [in] digest       : Digest corresponding to the certificate chain
 * @param [in] chain        : Certificate chain to check, at most 4096 bytes
 * @param [in] chain_length : Certificate chain length
 * @param [out] is_valid    : 1 if the certificate chain can be validated
 * @param [out] is_blocked  : 1 if the chain is valid but one of the certificate is blocked
 * @param [out] id          : if is_known and !is_blocked then contains the device handle
 *
 * @return 0 on SUCCESS else -1
 */
int usb_policy_engine_check_cert_chain(const uint32_t route,
				       const uint8_t *const digest,
				       const uint8_t *const chain,
				       const size_t chain_len,
				       uint8_t *is_valid, uint8_t *is_blocked,
				       uint32_t *id);

/**
 * @brief Remove a device from the policy engine
 *
 * Context: task context, might sleep.
 *
 * Possible errors:
 *  - EINVAL   : if digest is NULL
 *  - ECOMM    : if no userspace policy engine is available
 *                  or already userspace is busy
 *                  or message transmission failed
 *  - ENOMEM   : if message creation failed
 *  - EMSGSIZE : if message creation failed
 *
 * @param [in] id : Device handle
 *
 * @return 0 on SUCCESS else -1
 */
int usb_policy_engine_remove_dev(const uint32_t id);

/**
 * @brief Generate a nonce for the authentication challenge
 *
 * Context: task context, might sleep.
 *
 * Possible errors:
 *  - EINVAL   : if digest is NULL
 *  - ECOMM    : if no userspace policy engine is available
 *                  or already userspace is busy
 *                  or message transmission failed
 *  - ENOMEM   : if message creation failed
 *  - EMSGSIZE : if message creation failed
 *
 * @param [in] id     : device ID
 * @param [out] nonce : 32 bytes nonce buffer, caller allocated
 *
 * @return 0 on SUCCESS else -1
 */
int usb_policy_engine_generate_challenge(const uint32_t id, uint8_t *nonce);

/**
 * @brief Validate the authentication challenge
 *
 * Context: task context, might sleep.
 *
 * Possible errors:
 *  - EINVAL   : if challenge, desc or bos is NULL or invalid parameter size
 *  - ECOMM    : if no userspace policy engine is available
 *                  or already userspace is busy
 *                  or message transmission failed
 *  - ENOMEM   : if message creation failed
 *  - EMSGSIZE : if message creation failed
 *
 * Challenge is the concatenation of : message (140B) | signature (64B)
 *
 * Check that the response challenge contains the right nonce
 * Check that the device signature is valid
 *
 * @param [in] id : device handle
 * @param [in] challenge : challenge response, must be 204 bytes
 * @param [in] desc      : device descriptor
 * @param [in] desc_size : descriptor size in bytes
 * @param [in] bos       : device BOS
 * @param [in] bos_size  : BOS size in bytes
 * @param [out] is_valid : 1 if the signature is valid, 0 otherwise
 *
 * @return 0 on SUCCESS else -1
 */
int usb_policy_engine_check_challenge(const uint32_t id,
				      const uint8_t *const challenge,
				      const uint8_t *const context,
				      const size_t context_size,
				      uint8_t *is_valid);

#endif /* __USB_CORE_AUTHENT_NETLINK_H_ */
