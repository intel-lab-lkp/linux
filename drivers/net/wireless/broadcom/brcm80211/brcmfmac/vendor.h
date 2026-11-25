// SPDX-License-Identifier: ISC
/*
 * Copyright (c) 2014 Broadcom Corporation
 */

#ifndef _vendor_h_
#define _vendor_h_

#define BROADCOM_OUI	0x001018

/**
 * enum brcmf_vndr_cmds - vendor commands of the Broadcom OUI
 *
 * @BRCMF_VNDR_CMDS_UNSPEC: not used.
 * @BRCMF_VNDR_CMDS_DCMD: firmware command, see struct brcmf_vndr_dcmd_hdr.
 * @BRCMF_VNDR_CMDS_AWDL_START: create the AWDL interface with
 *	%BRCMF_AWDL_ATTR_ADDR and start AWDL with the TLVs of
 *	%BRCMF_AWDL_ATTR_PAYLOAD and %BRCMF_AWDL_ATTR_OOB. Runs asynchronously,
 *	the result comes with %BRCMF_VNDR_EVTS_AWDL_STARTED.
 * @BRCMF_VNDR_CMDS_AWDL_STOP: stop AWDL and remove the AWDL interface. Runs
 *	asynchronously, the result comes with %BRCMF_VNDR_EVTS_AWDL_STOPPED.
 * @BRCMF_VNDR_CMDS_AWDL_SET_FRAMES: replace the TLVs of
 *	%BRCMF_AWDL_ATTR_PAYLOAD and %BRCMF_AWDL_ATTR_OOB.
 * @BRCMF_VNDR_CMDS_AWDL_SET_SYNC: synchronize to the master of
 *	%BRCMF_AWDL_ATTR_MASTER, %BRCMF_AWDL_ATTR_MASTER_METRIC and
 *	%BRCMF_AWDL_ATTR_MASTER_DISTANCE. Without %BRCMF_AWDL_ATTR_MASTER the
 *	device is its own master.
 * @BRCMF_VNDR_CMDS_AWDL_PEER_ADD: add the peer %BRCMF_AWDL_ATTR_ADDR, data
 *	frames to it are sent from then on. %BRCMF_AWDL_ATTR_PEER_IES gives its
 *	HT and VHT capabilities, sent again it updates them.
 * @BRCMF_VNDR_CMDS_AWDL_PEER_DEL: remove the peer %BRCMF_AWDL_ATTR_ADDR.
 * @BRCMF_VNDR_CMDS_LAST: number of vendor commands plus one.
 *
 * The AWDL commands are sent to the primary interface. Election, peer
 * discovery and services are left to user space, the firmware sends and
 * receives the AWDL frames and data.
 */
enum brcmf_vndr_cmds {
	BRCMF_VNDR_CMDS_UNSPEC,
	BRCMF_VNDR_CMDS_DCMD,
	BRCMF_VNDR_CMDS_AWDL_START,
	BRCMF_VNDR_CMDS_AWDL_STOP,
	BRCMF_VNDR_CMDS_AWDL_SET_FRAMES,
	BRCMF_VNDR_CMDS_AWDL_SET_SYNC,
	BRCMF_VNDR_CMDS_AWDL_PEER_ADD,
	BRCMF_VNDR_CMDS_AWDL_PEER_DEL,
	BRCMF_VNDR_CMDS_LAST
};

/**
 * enum brcmf_vndr_evts - vendor events of the Broadcom OUI
 *
 * @BRCMF_VNDR_EVTS_UNSPEC: not used.
 * @BRCMF_VNDR_EVTS_AWDL_STARTED: result of %BRCMF_VNDR_CMDS_AWDL_START with
 *	%BRCMF_AWDL_ATTR_STATUS and, on success, %BRCMF_AWDL_ATTR_IFINDEX.
 * @BRCMF_VNDR_EVTS_AWDL_STOPPED: the AWDL interface was removed, with
 *	%BRCMF_AWDL_ATTR_STATUS. After %BRCMF_VNDR_CMDS_AWDL_STOP and before
 *	system sleep the status is 0 or the error of the removal. When the
 *	firmware removed the interface on its own it is -%ENOLINK.
 * @BRCMF_VNDR_EVTS_LAST: number of vendor events.
 *
 * The value of an event is its index in the vendor events and its vendor
 * subcommand.
 */
enum brcmf_vndr_evts {
	BRCMF_VNDR_EVTS_UNSPEC,
	BRCMF_VNDR_EVTS_AWDL_STARTED,
	BRCMF_VNDR_EVTS_AWDL_STOPPED,
	BRCMF_VNDR_EVTS_LAST
};

/**
 * enum brcmf_awdl_attrs - attributes of the AWDL vendor commands and events
 *
 * @BRCMF_AWDL_ATTR_UNSPEC: not used.
 * @BRCMF_AWDL_ATTR_ADDR: MAC address, of the AWDL interface or of a peer.
 * @BRCMF_AWDL_ATTR_PAYLOAD: AWDL TLVs for the firmware parameter
 *	awdl_payload.
 * @BRCMF_AWDL_ATTR_OOB: AWDL TLVs for the firmware parameter
 *	awdl_oob_af_auto.
 * @BRCMF_AWDL_ATTR_MASTER: MAC address of the master (u8[6]).
 * @BRCMF_AWDL_ATTR_MASTER_METRIC: metric of the master (u32).
 * @BRCMF_AWDL_ATTR_MASTER_DISTANCE: hops from this device to the master (u8).
 * @BRCMF_AWDL_ATTR_STATUS: 0 or a negative error code (s32).
 * @BRCMF_AWDL_ATTR_IFINDEX: interface index of the AWDL interface (u32).
 * @BRCMF_AWDL_ATTR_PEER_IES: HT and VHT capabilities elements of a peer,
 *	built from the AWDL TLVs it sends, as macOS passes them.
 * @__BRCMF_AWDL_ATTR_AFTER_LAST: internal use.
 * @BRCMF_AWDL_ATTR_MAX: highest attribute.
 */
enum brcmf_awdl_attrs {
	BRCMF_AWDL_ATTR_UNSPEC,

	BRCMF_AWDL_ATTR_ADDR,
	BRCMF_AWDL_ATTR_PAYLOAD,
	BRCMF_AWDL_ATTR_OOB,
	BRCMF_AWDL_ATTR_MASTER,
	BRCMF_AWDL_ATTR_MASTER_METRIC,
	BRCMF_AWDL_ATTR_MASTER_DISTANCE,
	BRCMF_AWDL_ATTR_STATUS,
	BRCMF_AWDL_ATTR_IFINDEX,
	BRCMF_AWDL_ATTR_PEER_IES,

	__BRCMF_AWDL_ATTR_AFTER_LAST,
	BRCMF_AWDL_ATTR_MAX = __BRCMF_AWDL_ATTR_AFTER_LAST - 1
};

/**
 * enum brcmf_nlattrs - nl80211 message attributes
 *
 * @BRCMF_NLATTR_LEN: message body length
 * @BRCMF_NLATTR_DATA: message body
 */
enum brcmf_nlattrs {
	BRCMF_NLATTR_UNSPEC,

	BRCMF_NLATTR_LEN,
	BRCMF_NLATTR_DATA,

	__BRCMF_NLATTR_AFTER_LAST,
	BRCMF_NLATTR_MAX = __BRCMF_NLATTR_AFTER_LAST - 1
};

/**
 * struct brcmf_vndr_dcmd_hdr - message header for cfg80211 vendor command dcmd
 *				support
 *
 * @cmd: common dongle cmd definition
 * @len: length of expecting return buffer
 * @offset: offset of data buffer
 * @set: get or set request(optional)
 * @magic: magic number for verification
 */
struct brcmf_vndr_dcmd_hdr {
	uint cmd;
	int len;
	uint offset;
	uint set;
	uint magic;
};

extern const struct wiphy_vendor_command brcmf_vendor_cmds[];
extern const struct nl80211_vendor_cmd_info brcmf_vendor_events[];

#endif /* _vendor_h_ */
