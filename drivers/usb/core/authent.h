/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SPDX-FileCopyrightText: (C) 2025 ANSSI
 *
 * USB Authentication protocol definition
 *
 * Author: Luc Bonnafoux <luc.bonnafoux@ssi.gouv.fr>
 * Author: Nicolas Bouchinet <nicolas.bouchinet@ssi.gouv.fr>
 *
 */

#ifndef __USB_CORE_AUTHENT_H_
#define __USB_CORE_AUTHENT_H_

#include <linux/types.h>
#include <linux/usb.h>
#include <linux/usb/ch11.h>
#include <linux/usb/hcd.h>

/* From USB Type-C Authentication spec, Table 5-2 */
#define USB_AUTHENT_CAP_TYPE 0x0e

/* From USB Security Foundation spec, Table 5-2 */
#define USB_SECURITY_PROTOCOL_VERSION 0x10

#define AUTH_IN 0x18
#define AUTH_OUT 0x19

/* USB_DT_AUTHENTICATION_CAP */
struct usb_authent_cap_descriptor {
	__u8  bLength;
	__u8  bDescriptorType;
	__u8  bDevCapabilityType; /* Shall be set to USB_AUTHENT_CAP_TYPE */
	/*
	 * bit 0: set to 1 if firmware can be updated
	 * bit 1: set to 1 to indicate the Device changes interface when updated
	 * bits 2-7: reserved, set to 0
	 */
	__u8  bmAttributes;
	__u8  bcdProtocolVersion; /* Shall be set to USB_SECURITY_PROTOCOL_VERSION */
	__u8  bcdCapability; /* Set to 0x01 */

} __packed;

/* Certificate chain header, Table 3-1 */
struct usb_cert_chain_hd {
	__u16 length; /* Chain total length including header, little endian */
	__u16 reserved; /* Shall be set to zero */
	__u8 rootHash[32]; /* Hash of root certificate, big endian */
} __packed;

/* From USB Security Foundation spec, Table 5-3 and Table 5-9 */
#define USB_AUTHENT_DIGEST_RESP_TYPE 0x01
#define USB_AUTHENT_CERTIFICATE_RESP_TYPE 0x02
#define USB_AUTHENT_CHALLENGE_RESP_TYPE 0x03
#define USB_AUTHENT_ERROR_TYPE 0x7f
#define USB_AUTHENT_DIGEST_REQ_TYPE 0x81
#define USB_AUTHENT_CERTIFICATE_REQ_TYPE 0x82
#define USB_AUTHENT_CHALLENGE_REQ_TYPE 0x83

#define USB_AUTH_DIGEST_SIZE 32
#define USB_AUTH_CHALL_SIZE 32

#define USB_AUTH_CHAIN_HEADER_SIZE 36

/* USB Authentication GET_DIGEST Request Header */
struct usb_authent_digest_req_hd {
	__u8 protocolVersion; /* Shall be set to USB_SECURITY_PROTOCOL_VERSION */
	__u8 messageType; /* Shall be set to USB_AUTHENT_DIGEST_REQ_TYPE */
	__u8 param1; /* Reserved */
	__u8 param2; /* Reserved */
} __packed;

/* USB Authentication GET_CERTIFICATE Request Header */
struct usb_authent_certificate_req_hd {
	__u8 protocolVersion; /* Shall be set to USB_SECURITY_PROTOCOL_VERSION */
	__u8 messageType; /* Shall be set to USB_AUTHENT_CERTIFICATE_REQ_TYPE */
	__u8 certChainSlotNumber; /* Must be between 0 and 7 inclusive */
	__u8 param2; /* Reserved */
} __packed;

/* USB Authentication GET_CERTIFICATE Request */
struct usb_authent_certificate_req {
	__u8 protocolVersion; /* Shall be set to USB_SECURITY_PROTOCOL_VERSION */
	__u8 messageType; /* Shall be set to USB_AUTHENT_CERTIFICATE_REQ_TYPE */
	__u8 certChainSlotNumber; /* Must be between 0 and 7 inclusive */
	__u8 param2; /* Reserved */
	__u16 offset; /* Read index of Certificate Chain in bytes and little endian*/
	__u16 length; /* Length of read request, little endian */
} __packed;

/* USB Authentication CHALLENGE Request Header */
struct usb_authent_challenge_req_hd {
	__u8 protocolVersion; /* Shall be set to USB_SECURITY_PROTOCOL_VERSION */
	__u8 messageType; /* Shall be set to USB_AUTHENT_CHALLENGE_REQ_TYPE */
	__u8 certChainSlotNumber; /* Must be between 0 and 7 inclusive */
	__u8 param2; /* Reserved */
} __packed;

/* USB Authentication CHALLENGE Request Header */
struct usb_authent_challenge_req {
	__u8 protocolVersion; /* Shall be set to USB_SECURITY_PROTOCOL_VERSION */
	__u8 messageType; /* Shall be set to USB_AUTHENT_CHALLENGE_REQ_TYPE */
	__u8 certChainSlotNumber; /* Must be between 0 and 7 inclusive */
	__u8 param2; /* Reserved */
	__u32 nonce; /* Random Nonce chosen for the challenge */
} __packed;

/* USB Authentication DIGEST response Header */
struct usb_authent_digest_resp {
	__u8 protocolVersion; /* Shall be set to USB_SECURITY_PROTOCOL_VERSION */
	__u8 messageType; /* Shall be set to USB_AUTHENT_DIGEST_RESP_TYPE */
	__u8 capability; /* Shall be set to 0x01 */
	__u8 slotMask; /* Bit set to 1 if slot is set, indicates number of digests */
	__u8 digests[8][32]; /* List of digests */
} __packed;

/* USB Authentication CERTIFICATE response Header */
struct usb_authent_certificate_resp_hd {
	__u8 protocolVersion; /* Shall be set to USB_SECURITY_PROTOCOL_VERSION */
	__u8 messageType; /* Shall be set to USB_AUTHENT_CERTIFICATE_RESP_TYPE */
	__u8 slotNumber; /* Slot number of certificate chain returned */
	__u8 param2; /* Reserved */
} __packed;

/* USB Authentication CHALLENGE response Header */
struct usb_authent_challenge_resp_hd {
	__u8 protocolVersion; /* Shall be set to USB_SECURITY_PROTOCOL_VERSION */
	__u8 messageType; /* Shall be set to USB_AUTHENT_CHALLENGE_RESP_TYPE */
	__u8 slotNumber; /* Slot number of certificate chain returned */
	__u8 slotMask; /* Bit set to 1 if slot is set */
} __packed;

/* USB Authentication CHALLENGE response */
struct usb_authent_challenge_resp {
	__u8 protocolVersion; /* Shall be set to USB_SECURITY_PROTOCOL_VERSION */
	__u8 messageType; /* Shall be set to USB_AUTHENT_CHALLENGE_RESP_TYPE */
	__u8 slotNumber; /* Slot number of certificate chain returned */
	__u8 slotMask; /* Bit set to 1 if slot is set */
	__u8 minProtocolVersion;
	__u8 maxProtocolVersion;
	__u8 capabilities; /* Shall be set to 0x01 */
	__u8 orgName; /* Organisation Name, USB-IF: 0 */
	__u32 certChainHash; /* SHA256 digest of certificate chain, big endian */
	__u32 salt; /* Chosen by responder */
	__u32 contextHash; /* SHA256 digest of product information, big endian */
	__u64 signature; /* ECDSA signature of request and response */
} __packed;

/* USB Authentication error codes, Foundation Table 5-18 */
#define USB_AUTHENT_INVALID_REQUEST_ERROR 0x01
#define USB_AUTHENT_UNSUPPORTED_PROTOCOL_ERROR 0x02
#define USB_AUTHENT_BUSY_ERROR 0x03
#define USB_AUTHENT_UNSPECIFIED_ERROR 0x04

/* USB Authentication response header */
struct usb_authent_error_resp_hd {
	__u8 protocolVersion; /* Shall be set to USB_SECURITY_PROTOCOL_VERSION */
	__u8 messageType; /* Shall be set to USB_AUTHENT_ERROR_TYPE */
	__u8 errorCode;
	__u8 errorData;
} __packed;

int usb_authenticate_device(struct usb_device *dev);

#endif /* __USB_CORE_AUTHENT_H_ */
