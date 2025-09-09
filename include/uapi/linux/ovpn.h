/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/ovpn.yaml */
/* YNL-GEN uapi header */

#ifndef _UAPI_LINUX_OVPN_H
#define _UAPI_LINUX_OVPN_H

#define OVPN_FAMILY_NAME	"ovpn"
#define OVPN_FAMILY_VERSION	1

#define OVPN_NONCE_TAIL_SIZE	8

enum ovpn_cipher_alg {
	OVPN_CIPHER_ALG_NONE,
	OVPN_CIPHER_ALG_AES_GCM,
	OVPN_CIPHER_ALG_CHACHA20_POLY1305,
};

enum ovpn_del_peer_reason {
	OVPN_DEL_PEER_REASON_TEARDOWN,
	OVPN_DEL_PEER_REASON_USERSPACE,
	OVPN_DEL_PEER_REASON_EXPIRED,
	OVPN_DEL_PEER_REASON_TRANSPORT_ERROR,
	OVPN_DEL_PEER_REASON_TRANSPORT_DISCONNECT,
};

enum ovpn_key_slot {
	OVPN_KEY_SLOT_PRIMARY,
	OVPN_KEY_SLOT_SECONDARY,
};

/**
 * enum ovpn_peer
 * @OVPN_A_PEER_ID: The unique ID of the peer in the device context. To be used
 *   to identify peers during operations for a specific device
 * @OVPN_A_PEER_REMOTE_IPV4: The remote IPv4 address of the peer
 * @OVPN_A_PEER_REMOTE_IPV6: The remote IPv6 address of the peer
 * @OVPN_A_PEER_REMOTE_IPV6_SCOPE_ID: The scope id of the remote IPv6 address
 *   of the peer (RFC2553)
 * @OVPN_A_PEER_REMOTE_PORT: The remote port of the peer
 * @OVPN_A_PEER_SOCKET: The socket to be used to communicate with the peer
 * @OVPN_A_PEER_SOCKET_NETNSID: The ID of the netns the socket assigned to this
 *   peer lives in
 * @OVPN_A_PEER_VPN_IPV4: The IPv4 address assigned to the peer by the server
 * @OVPN_A_PEER_VPN_IPV6: The IPv6 address assigned to the peer by the server
 * @OVPN_A_PEER_LOCAL_IPV4: The local IPv4 to be used to send packets to the
 *   peer (UDP only)
 * @OVPN_A_PEER_LOCAL_IPV6: The local IPv6 to be used to send packets to the
 *   peer (UDP only)
 * @OVPN_A_PEER_LOCAL_PORT: The local port to be used to send packets to the
 *   peer (UDP only)
 * @OVPN_A_PEER_KEEPALIVE_INTERVAL: The number of seconds after which a keep
 *   alive message is sent to the peer
 * @OVPN_A_PEER_KEEPALIVE_TIMEOUT: The number of seconds from the last activity
 *   after which the peer is assumed dead
 * @OVPN_A_PEER_DEL_REASON: The reason why a peer was deleted
 * @OVPN_A_PEER_VPN_RX_BYTES: Number of bytes received over the tunnel
 * @OVPN_A_PEER_VPN_TX_BYTES: Number of bytes transmitted over the tunnel
 * @OVPN_A_PEER_VPN_RX_PACKETS: Number of packets received over the tunnel
 * @OVPN_A_PEER_VPN_TX_PACKETS: Number of packets transmitted over the tunnel
 * @OVPN_A_PEER_LINK_RX_BYTES: Number of bytes received at the transport level
 * @OVPN_A_PEER_LINK_TX_BYTES: Number of bytes transmitted at the transport
 *   level
 * @OVPN_A_PEER_LINK_RX_PACKETS: Number of packets received at the transport
 *   level
 * @OVPN_A_PEER_LINK_TX_PACKETS: Number of packets transmitted at the transport
 *   level
 */
enum {
	OVPN_A_PEER_ID = 1,
	OVPN_A_PEER_REMOTE_IPV4,
	OVPN_A_PEER_REMOTE_IPV6,
	OVPN_A_PEER_REMOTE_IPV6_SCOPE_ID,
	OVPN_A_PEER_REMOTE_PORT,
	OVPN_A_PEER_SOCKET,
	OVPN_A_PEER_SOCKET_NETNSID,
	OVPN_A_PEER_VPN_IPV4,
	OVPN_A_PEER_VPN_IPV6,
	OVPN_A_PEER_LOCAL_IPV4,
	OVPN_A_PEER_LOCAL_IPV6,
	OVPN_A_PEER_LOCAL_PORT,
	OVPN_A_PEER_KEEPALIVE_INTERVAL,
	OVPN_A_PEER_KEEPALIVE_TIMEOUT,
	OVPN_A_PEER_DEL_REASON,
	OVPN_A_PEER_VPN_RX_BYTES,
	OVPN_A_PEER_VPN_TX_BYTES,
	OVPN_A_PEER_VPN_RX_PACKETS,
	OVPN_A_PEER_VPN_TX_PACKETS,
	OVPN_A_PEER_LINK_RX_BYTES,
	OVPN_A_PEER_LINK_TX_BYTES,
	OVPN_A_PEER_LINK_RX_PACKETS,
	OVPN_A_PEER_LINK_TX_PACKETS,

	__OVPN_A_PEER_MAX,
	OVPN_A_PEER_MAX = (__OVPN_A_PEER_MAX - 1)
};

/**
 * enum ovpn_keyconf
 * @OVPN_A_KEYCONF_PEER_ID: The unique ID of the peer in the device context. To
 *   be used to identify peers during key operations
 * @OVPN_A_KEYCONF_SLOT: The slot where the key should be stored
 * @OVPN_A_KEYCONF_KEY_ID: The unique ID of the key in the peer context. Used
 *   to fetch the correct key upon decryption
 * @OVPN_A_KEYCONF_CIPHER_ALG: The cipher to be used when communicating with
 *   the peer
 * @OVPN_A_KEYCONF_ENCRYPT_DIR: Key material for encrypt direction
 * @OVPN_A_KEYCONF_DECRYPT_DIR: Key material for decrypt direction
 */
enum {
	OVPN_A_KEYCONF_PEER_ID = 1,
	OVPN_A_KEYCONF_SLOT,
	OVPN_A_KEYCONF_KEY_ID,
	OVPN_A_KEYCONF_CIPHER_ALG,
	OVPN_A_KEYCONF_ENCRYPT_DIR,
	OVPN_A_KEYCONF_DECRYPT_DIR,

	__OVPN_A_KEYCONF_MAX,
	OVPN_A_KEYCONF_MAX = (__OVPN_A_KEYCONF_MAX - 1)
};

/**
 * enum ovpn_keydir
 * @OVPN_A_KEYDIR_CIPHER_KEY: The actual key to be used by the cipher
 * @OVPN_A_KEYDIR_NONCE_TAIL: Random nonce to be concatenated to the packet ID,
 *   in order to obtain the actual cipher IV
 */
enum {
	OVPN_A_KEYDIR_CIPHER_KEY = 1,
	OVPN_A_KEYDIR_NONCE_TAIL,

	__OVPN_A_KEYDIR_MAX,
	OVPN_A_KEYDIR_MAX = (__OVPN_A_KEYDIR_MAX - 1)
};

/**
 * enum ovpn_ovpn
 * @OVPN_A_IFINDEX: Index of the ovpn interface to operate on
 * @OVPN_A_PEER: The peer object containing the attributed of interest for the
 *   specific operation
 * @OVPN_A_KEYCONF: Peer specific cipher configuration
 */
enum {
	OVPN_A_IFINDEX = 1,
	OVPN_A_PEER,
	OVPN_A_KEYCONF,

	__OVPN_A_MAX,
	OVPN_A_MAX = (__OVPN_A_MAX - 1)
};

enum {
	OVPN_CMD_PEER_NEW = 1,
	OVPN_CMD_PEER_SET,
	OVPN_CMD_PEER_GET,
	OVPN_CMD_PEER_DEL,
	OVPN_CMD_PEER_DEL_NTF,
	OVPN_CMD_KEY_NEW,
	OVPN_CMD_KEY_GET,
	OVPN_CMD_KEY_SWAP,
	OVPN_CMD_KEY_SWAP_NTF,
	OVPN_CMD_KEY_DEL,

	__OVPN_CMD_MAX,
	OVPN_CMD_MAX = (__OVPN_CMD_MAX - 1)
};

#define OVPN_MCGRP_PEERS	"peers"

#endif /* _UAPI_LINUX_OVPN_H */
