/* SPDX-License-Identifier: GPL-2.0 */
#include <stdint.h>

#define TPM_ALG_SHA_384_SIZE 48

struct cert_chain {
	void *chain;
	size_t len;
};

int spdm_get_digests(int dev_no, uint8_t digest[TPM_ALG_SHA_384_SIZE]);
int spdm_get_certificate(int dev_no, struct cert_chain *c);

