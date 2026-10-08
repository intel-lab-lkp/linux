/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/pon.yaml */
/* YNL-GEN uapi header */
/* To regenerate run: tools/net/ynl/ynl-regen.sh */

#ifndef _UAPI_LINUX_PON_H
#define _UAPI_LINUX_PON_H

#define PON_FAMILY_NAME		"pon"
#define PON_FAMILY_VERSION	1

/*
 * PON operating mode.
 */
enum pon_mode {
	PON_MODE_GPON,
	PON_MODE_XG_PON,
	PON_MODE_XGS_PON,
};

/*
 * ITU-T G.9807.1 / G.984.3 ONU activation state. O5 is the only state in which
 * the OMCC carries traffic. G.9807.1 has one Serial Number state, O2-3 (Table
 * C.12.1), reported as o2. Only a G.984.3 device reports o3.
 */
enum pon_ploam_state {
	PON_PLOAM_STATE_UNKNOWN,
	PON_PLOAM_STATE_O1,
	PON_PLOAM_STATE_O2,
	PON_PLOAM_STATE_O3,
	PON_PLOAM_STATE_O4,
	PON_PLOAM_STATE_O5,
	PON_PLOAM_STATE_O6,
	PON_PLOAM_STATE_O7,
};

/*
 * GEM port direction. The values are those of the direction attribute of the
 * GEM port network CTP managed entity of ITU-T G.988: UNI-to-ANI (1),
 * ANI-to-UNI (2) or bidirectional (3). An OMCI stack passes the value through.
 */
enum pon_gem_dir {
	PON_GEM_DIR_UPSTREAM = 1,
	PON_GEM_DIR_DOWNSTREAM,
	PON_GEM_DIR_BIDIR,
};

/**
 * enum pon_gem_key_ring - Whether a GEM port is encrypted and with which keys,
 *   as the encryption key ring attribute of the GEM port network CTP managed
 *   entity of ITU-T G.988 clause 9.2.3 defines it.
 * @PON_GEM_KEY_RING_NONE: No encryption. Upstream is sent with key index 0.
 * @PON_GEM_KEY_RING_UNICAST: Unicast encryption in both directions, with the
 *   keys the ONU generates.
 * @PON_GEM_KEY_RING_BROADCAST: Broadcast encryption, with the keys the OLT
 *   distributes over the OMCI.
 * @PON_GEM_KEY_RING_UNICAST_DOWNSTREAM: Unicast encryption of the downstream
 *   only, with the keys the ONU generates.
 */
enum pon_gem_key_ring {
	PON_GEM_KEY_RING_NONE,
	PON_GEM_KEY_RING_UNICAST,
	PON_GEM_KEY_RING_BROADCAST,
	PON_GEM_KEY_RING_UNICAST_DOWNSTREAM,
};

/*
 * VLAN tag state an upstream classifier rule matches.
 */
enum pon_gem_map_tag {
	PON_GEM_MAP_TAG_UNTAGGED,
	PON_GEM_MAP_TAG_TAGGED,
};

enum {
	PON_A_DEV_ID = 1,
	PON_A_DEV_IFINDEX,
	PON_A_DEV_MODE,
	PON_A_DEV_MODES_CAP,
	PON_A_DEV_SERIAL,
	PON_A_DEV_REGISTRATION_ID,
	PON_A_DEV_PLOAM_STATE,
	PON_A_DEV_MAX_TCONTS,
	PON_A_DEV_MAX_GEMS,
	PON_A_DEV_ENABLE,

	__PON_A_DEV_MAX,
	PON_A_DEV_MAX = (__PON_A_DEV_MAX - 1)
};

enum {
	PON_A_TCONT_DEV_ID = 1,
	PON_A_TCONT_INDEX,
	PON_A_TCONT_ALLOC_ID,

	__PON_A_TCONT_MAX,
	PON_A_TCONT_MAX = (__PON_A_TCONT_MAX - 1)
};

enum {
	PON_A_GEM_DEV_ID = 1,
	PON_A_GEM_ID,
	PON_A_GEM_DIR,
	PON_A_GEM_TCONT_INDEX,
	PON_A_GEM_KEY_RING,

	__PON_A_GEM_MAX,
	PON_A_GEM_MAX = (__PON_A_GEM_MAX - 1)
};

enum {
	PON_A_GEM_MAP_DEV_ID = 1,
	PON_A_GEM_MAP_GEM_ID,
	PON_A_GEM_MAP_TAG,
	PON_A_GEM_MAP_VID,
	PON_A_GEM_MAP_PBIT,
	PON_A_GEM_MAP_DSCP,

	__PON_A_GEM_MAP_MAX,
	PON_A_GEM_MAP_MAX = (__PON_A_GEM_MAP_MAX - 1)
};

enum {
	PON_A_OMCI_DEV_ID = 1,
	PON_A_OMCI_PDU,

	__PON_A_OMCI_MAX,
	PON_A_OMCI_MAX = (__PON_A_OMCI_MAX - 1)
};

enum {
	PON_CMD_DEV_GET = 1,
	PON_CMD_DEV_SET,
	PON_CMD_DEV_ADD_NTF,
	PON_CMD_DEV_DEL_NTF,
	PON_CMD_DEV_CHANGE_NTF,
	PON_CMD_PLOAM_NTF,
	PON_CMD_TCONT_GET,
	PON_CMD_TCONT_SET,
	PON_CMD_TCONT_DEL,
	PON_CMD_TCONT_ADD_NTF,
	PON_CMD_TCONT_CHANGE_NTF,
	PON_CMD_TCONT_DEL_NTF,
	PON_CMD_GEM_GET,
	PON_CMD_GEM_NEW,
	PON_CMD_GEM_DEL,
	PON_CMD_GEM_ADD_NTF,
	PON_CMD_GEM_DEL_NTF,
	PON_CMD_GEM_MAP_GET,
	PON_CMD_GEM_MAP_NEW,
	PON_CMD_GEM_MAP_DEL,
	PON_CMD_GEM_MAP_ADD_NTF,
	PON_CMD_GEM_MAP_DEL_NTF,
	PON_CMD_OMCI_REGISTER,
	PON_CMD_OMCI_TX,
	PON_CMD_OMCI_NTF,

	__PON_CMD_MAX,
	PON_CMD_MAX = (__PON_CMD_MAX - 1)
};

#define PON_MCGRP_MGMT	"mgmt"
#define PON_MCGRP_STATE	"state"

#endif /* _UAPI_LINUX_PON_H */
