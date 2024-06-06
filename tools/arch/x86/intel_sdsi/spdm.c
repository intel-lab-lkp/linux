// SPDX-License-Identifier: GPL-2.0
/*
 * spdm: Lightweight Security Protocol and Data Model (SPDM) specification
 * support code for performing attestation commands using the Intel On
 * Demand driver ioctl interface. Intel On Demand currently supports
 * SPDM version 1.0
 *
 * See the SPDM v1.0 specification at:
 * https://www.dmtf.org/sites/default/files/standards/documents/DSP0274_1.0.1.pdf
 *
 * Copyright (C) 2024 Intel Corporation. All rights reserved.
 */

#include<linux/bits.h>

#include<fcntl.h>
#include<stdio.h>
#include<stdlib.h>
#include<stdint.h>
#include<string.h>
#include<unistd.h>
#include<sys/ioctl.h>
#include "spdm.h"
#include "intel_sdsi.h"

// SPDM constants
#define SPDM_VERSION			0x10
#define SPDM_REQUEST			0x80
#define SPDM_ERROR			0x7f

// SPDM request codes
#define SPDM_GET_VERSION		0x84
#define SPDM_GET_CAPABILITIES		0xE1
#define SPDM_NEGOTIATE_ALGORITHMS	0xE3
#define SPDM_GET_DIGESTS		0x81
#define SPDM_GET_CERTIFICATE		0x82
#define SPDM_CHALLENGE			0x83
#define SPDM_GET_MEASUREMENTS		0xE0

#define SDSI_DEV_PATH			"/dev/isdsi"

#define SPDM_RSVD			0

#ifndef __packed
#define __packed __attribute__((packed))
#endif

struct spdm_header {
	uint8_t version;
	uint8_t code;
	uint8_t param1;
	uint8_t param2;
};

static int error_response(struct spdm_header *response)
{
	if (response->code != SPDM_ERROR)
		fprintf(stderr, "ERROR: Unrecognized SPDM response\n");

	switch (response->param1) {
	case 0x00:
	case 0x02:
	case 0x06:
	case 0x08 ... 0x40:
	case 0x44 ... 0xfe:
		fprintf(stderr, "SPDM RSP ERROR: Reserved.\n");
		break;
	case 0x01:
		fprintf(stderr, "SPDM RSP ERROR: One or more request fields are invalid.\n");
		break;
	case 0x03:
		fprintf(stderr, "SPDM RSP ERROR: The Responder received the request message\n");
		fprintf(stderr, "and the Responder decided to ignore the request message\n");
		fprintf(stderr, "but the Responder may be able to process the request message\n");
		fprintf(stderr, "if the request message is sent again in the future.\n");
		break;
	case 0x04:
		fprintf(stderr, "SPDM RSP ERROR: The Responder received an unexpected request\n");
		fprintf(stderr, "message. For example, CHALLENGE before NEGOTIATE_ALGORITHMS.\n");
		break;
	case 0x05:
		fprintf(stderr, "SPDM RSP ERROR: Unspecified error occurred.\n");
		break;
	case 0x07:
		fprintf(stderr, "SPDM RSP ERROR: The RequestResponseCode in the request\n");
		fprintf(stderr, "message is unsupported.\n");
		break;
	case 0x41:
		fprintf(stderr, "SPDM RSP ERROR: Requested SPDM Major Version is not\n");
		fprintf(stderr, "supported.\n");
		break;
	case 0x42:
		fprintf(stderr, "SPDM RSP ERROR: See the RESPONSE_IF_READY request message.\n");
		break;
	case 0x43:
		fprintf(stderr, "SPDM RSP ERROR: Responder is requesting Requester to reissue\n");
		fprintf(stderr, "GET_VERSION to resynchronize.\n");
		break;
	case 0xFF:
		fprintf(stderr, "SPDM RSP ERROR: Vendor or Other Standards defined.\n");
		break;
	}

	return -1;
}

static int sdsi_process_ioctl(int ioctl_no, void *info, uint8_t dev_no)
{
	char pathname[14];
	int fd, ret;

	ret = snprintf(pathname, 14, "%s%d", SDSI_DEV_PATH, dev_no);
	if (ret < 0)
		return ret;

	fd = open(pathname, O_RDONLY);
	if (fd < 0)
		return fd;

	ret = ioctl(fd, ioctl_no, info);
	if (ret)
		perror("Failed to process ioctl");

	close(fd);

	return ret;
}

static int
sdsi_process_spdm(void *request, void *response, int req_size, uint32_t rsp_size,
		  int dev_no)
{
	struct sdsi_spdm_command *command;
	struct sdsi_spdm_message *message = request;
	uint8_t request_code;
	int ret;

	command = malloc(sizeof(*command));
	if (!command) {
		perror("malloc");
		return -1;
	}

	command->size = req_size;
	command->message = *message;
	request_code = command->message.request_response_code;

	ret = sdsi_process_ioctl(SDSI_IF_SPDM_COMMAND, command, dev_no);
	if (ret)
		goto free_command;

	if (command->size < sizeof(struct spdm_header)) {
		fprintf(stderr, "Bad SPDM message size\n");
		ret = -1;
		goto free_command;
	}

	if (command->message.request_response_code != (request_code & ~SPDM_REQUEST)) {
		ret = error_response((struct spdm_header *)&command->message);
		goto free_command;
	}

	if (response) {
		if (command->size > rsp_size) {
			fprintf(stderr, "SPDM response buffer too small\n");
			ret = -1;
			goto free_command;
		}

		memcpy(response, &command->message, command->size);
	}

free_command:
	free(command);
	return ret;
}

struct version_number_entry {
	uint8_t alpha:4;
	uint8_t update_version_number:4;
	union {
		uint8_t version;
		struct {
			uint8_t minor:4;
			uint8_t major:4;
		};
	};
} __packed;

struct get_version_response {
	struct spdm_header header;
	uint16_t reserved:8;
	uint16_t version_number_entry_count:8;
	struct version_number_entry entry[10];
} __packed;

static int spdm_get_version(int dev_no)
{
	struct spdm_header request = {};
	struct get_version_response response = {};
	uint8_t version;
	int ret;

	request.version = SPDM_VERSION;
	request.code = SPDM_GET_VERSION;
	request.param1 = SPDM_RSVD;
	request.param2 = SPDM_RSVD;

	ret = sdsi_process_spdm(&request, &response, sizeof(request),
				sizeof(response), dev_no);
	if (ret) {
		fprintf(stderr, "Failed GET_VERSION\n");
		return ret;
	}

	if (!response.version_number_entry_count) {
		fprintf(stderr, "Bad GET_VERSION entry count\n");
		return -1;
	}

	version = response.entry[0].version;

	if (version != SPDM_VERSION) {
		fprintf(stderr, "Unsupported version 0x%x\n", SPDM_VERSION);
		return -1;
	}

	return 0;
}

static int spdm_get_capabilities(int dev_no)
{
	struct spdm_header request = {};
	int ret;

	request.version = SPDM_VERSION;
	request.code = SPDM_GET_CAPABILITIES;
	request.param1 = SPDM_RSVD;
	request.param2 = SPDM_RSVD;

	ret = sdsi_process_spdm(&request, NULL, sizeof(request), 0, dev_no);
	if (ret) {
		fprintf(stderr, "Failed GET_CAPABILITIES\n");
		return ret;
	}

	return 0;
}

struct spdm_negotiate_alg {
	struct spdm_header header;
	uint32_t	length:16;
	uint32_t	measurement_specification:8;
	uint32_t	reserved:8;
	uint32_t	base_asym_algo;
	uint32_t	base_hash_algo;
	uint32_t	reserved2[3];
	uint32_t	ext_asym_count:8;
	uint32_t	ext_hash_count:8;
	uint32_t	reserved3:16;
};

#define MEASUREMENT_SPEC_DMTF			BIT(0)
#define BASE_ASYM_ALG_ECDSA_ECC_NIST_P384	BIT(7)
#define BASE_HASH_ALG_SHA_384			BIT(1)

static int spdm_negotiate_algorithms(int dev_no)
{
	struct spdm_negotiate_alg request = {};
	int ret;

	request.header.version = SPDM_VERSION;
	request.header.code = SPDM_NEGOTIATE_ALGORITHMS;
	request.header.param1 = SPDM_RSVD;
	request.header.param2 = SPDM_RSVD;

	request.length = sizeof(request);
	request.measurement_specification = MEASUREMENT_SPEC_DMTF;
	request.base_asym_algo = BASE_ASYM_ALG_ECDSA_ECC_NIST_P384;
	request.base_hash_algo = BASE_HASH_ALG_SHA_384;

	ret = sdsi_process_spdm(&request, NULL, sizeof(request), 0, dev_no);
	if (ret) {
		fprintf(stderr, "Failed NEGOTIATE_ALGORITHMS\n");
		return ret;
	}

	return 0;
}

static int spdm_negotiate(int dev_no)
{
	int ret;

	ret = spdm_get_version(dev_no);
	if (ret)
		return ret;

	ret = spdm_get_capabilities(dev_no);
	if (ret)
		return ret;

	return spdm_negotiate_algorithms(dev_no);
}

struct get_digests_response {
	struct spdm_header header;
	uint8_t digest[TPM_ALG_SHA_384_SIZE];
};

#define SLOT_MASK(slot) BIT(slot)

int spdm_get_digests(int dev_no, uint8_t digest[TPM_ALG_SHA_384_SIZE])
{
	struct spdm_header request = {};
	struct get_digests_response response = {};
	int ret;

	ret = spdm_negotiate(dev_no);
	if (ret)
		return ret;

	request.version = SPDM_VERSION;
	request.code = SPDM_GET_DIGESTS;
	request.param1 = SPDM_RSVD;
	request.param2 = SPDM_RSVD;

	ret = sdsi_process_spdm(&request, &response, sizeof(request),
				sizeof(response), dev_no);
	if (ret) {
		fprintf(stderr, "Failed GET_DIGESTS\n");
		return ret;
	}

	if (!(response.header.param2 & SLOT_MASK(0))) {
		fprintf(stderr, "Error, Slot 0 not selected in GET_DIGESTS\n");
		return -1;
	}

	if (digest)
		memcpy(digest, response.digest, TPM_ALG_SHA_384_SIZE);

	return 0;
}

#define CERT_SLOT	0

struct get_cert_request {
	struct spdm_header header;
	uint16_t offset;
	uint16_t length;
};

struct get_cert_response {
	struct spdm_header header;
	uint16_t portion_length;
	uint16_t remainder_length;
	uint8_t certificate_chain[SDSI_SPDM_BUF_SIZE];
};

static int get_certificate_size(int dev_no)
{
	struct get_cert_request request = {};
	struct get_cert_response response = {};
	int ret;

	request.header.version = SPDM_VERSION;
	request.header.code = SPDM_GET_CERTIFICATE;
	request.header.param1 = CERT_SLOT;
	request.header.param2 = SPDM_RSVD;
	request.offset = 0;
	request.length = SDSI_SPDM_BUF_SIZE;

	ret = sdsi_process_spdm(&request, &response, sizeof(request),
				sizeof(response), dev_no);
	if (ret) {
		fprintf(stderr, "Error getting size during GET_CERTIFICATE\n");
		return ret;
	}

	return response.portion_length + response.remainder_length;
}

static int get_certificate_portion(int dev_no, uint16_t offset, uint16_t length,
				   uint16_t *portion_length, uint16_t *remainder_length,
				   uint8_t *cert_chain)
{
	struct get_cert_request request = {};
	struct get_cert_response response = {};
	int ret;

	request.header.version = SPDM_VERSION;
	request.header.code = SPDM_GET_CERTIFICATE;
	request.header.param1 = CERT_SLOT;
	request.header.param2 = SPDM_RSVD;
	request.offset = offset;
	request.length = length;

	ret = sdsi_process_spdm(&request, &response, sizeof(request),
				sizeof(response), dev_no);
	if (ret) {
		fprintf(stderr, "Failed GET_CERTIFICATE\n");
		return ret;
	}

	*portion_length = response.portion_length;
	*remainder_length = response.remainder_length;

	memcpy(cert_chain + offset, response.certificate_chain, *portion_length);

	return 0;
}

int spdm_get_certificate(int dev_no, struct cert_chain *c)
{
	uint16_t remainder_length;
	uint16_t old_remainder;
	uint16_t portion_length = 0;
	uint16_t offset = 0;
	uint16_t size;
	int ret;

	ret = spdm_negotiate(dev_no);
	if (ret)
		return ret;

	ret = spdm_get_digests(dev_no, NULL);
	if (ret)
		return ret;

	ret = get_certificate_size(dev_no);
	if (ret < 0)
		return ret;

	size = ret;

	c->chain = malloc(size);
	if (!c->chain) {
		perror("malloc");
		return -1;
	}

	remainder_length = size < SDSI_SPDM_BUF_SIZE ? size : SDSI_SPDM_BUF_SIZE;
	old_remainder = remainder_length;

	while (remainder_length) {
		uint16_t length;

		length = remainder_length < SDSI_SPDM_BUF_SIZE ?
				remainder_length : SDSI_SPDM_BUF_SIZE;
		offset += portion_length;

		ret = get_certificate_portion(dev_no, offset, length,
					      &portion_length,
					      &remainder_length,
					      c->chain);
		if (ret < 0)
			goto free_cert_chain;

		if (!(remainder_length < old_remainder)) {
			fprintf(stderr, "Bad GET_CERTIFICATE length\n");
			ret = -1;
			goto free_cert_chain;
		}

		old_remainder = remainder_length;
	}

	c->len = offset + portion_length;
	return 0;

free_cert_chain:
	free(c->chain);
	c->chain = NULL;
	return ret;
}
