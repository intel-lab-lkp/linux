// SPDX-License-Identifier: GPL-2.0
/*
 * AF_ALG AEAD round-trip prober.  The killswitch selftest uses this
 * to demonstrate that engaging a killswitch on af_alg_sendmsg
 * neuters AF_ALG operations (sendmsg returns -EPERM), mitigating
 * any AF_ALG-reachable bug whose exploit primitive runs from the
 * send path.
 *
 * Exit codes:
 *   0  AEAD round-trip succeeded (function intact)
 *   1  AEAD round-trip refused (mitigation engaged)
 *   2  setup error (no AF_ALG, missing aead/gcm(aes), etc.) -> SKIP
 *
 * Copyright (C) 2026 Sasha Levin <sashal@kernel.org>
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/if_alg.h>

#define KEY_LEN		16
#define IV_LEN		12
#define AAD_LEN		16
#define PT_LEN		64
#define TAG_LEN		16
#define EXPECTED_LEN	(AAD_LEN + PT_LEN + TAG_LEN)

#ifndef AF_ALG
#define AF_ALG		38
#endif
#ifndef SOL_ALG
#define SOL_ALG		279
#endif

int main(void)
{
	struct sockaddr_alg sa = {
		.salg_family = AF_ALG,
		.salg_type   = "aead",
		.salg_name   = "gcm(aes)",
	};
	unsigned char key[KEY_LEN] = { 0 };
	unsigned char iv[IV_LEN]   = { 0 };
	unsigned char buf[1024]    = { 0 };
	struct msghdr msg = { 0 };
	struct iovec iov;
	struct cmsghdr *cmsg;
	struct af_alg_iv *aiv;
	char cbuf[256] = { 0 };
	int *p_op, *p_assoclen;
	int sk, opfd;
	ssize_t n;

	sk = socket(AF_ALG, SOCK_SEQPACKET, 0);
	if (sk < 0) {
		fprintf(stderr, "AF_ALG socket: %s -- skip\n", strerror(errno));
		return 2;
	}
	if (bind(sk, (struct sockaddr *)&sa, sizeof(sa))) {
		fprintf(stderr, "bind aead/gcm(aes): %s -- skip\n",
			strerror(errno));
		close(sk);
		return 2;
	}
	if (setsockopt(sk, SOL_ALG, ALG_SET_KEY, key, KEY_LEN)) {
		fprintf(stderr, "ALG_SET_KEY: %s -- skip\n", strerror(errno));
		close(sk);
		return 2;
	}
	if (setsockopt(sk, SOL_ALG, ALG_SET_AEAD_AUTHSIZE, NULL, TAG_LEN)) {
		fprintf(stderr, "ALG_SET_AEAD_AUTHSIZE: %s -- skip\n",
			strerror(errno));
		close(sk);
		return 2;
	}

	opfd = accept(sk, NULL, 0);
	if (opfd < 0) {
		fprintf(stderr, "accept: %s -- skip\n", strerror(errno));
		close(sk);
		return 2;
	}

	/* control message: ENCRYPT op + IV + assoclen */
	msg.msg_control    = cbuf;
	msg.msg_controllen = CMSG_SPACE(sizeof(int))
			   + CMSG_SPACE(sizeof(*aiv) + IV_LEN)
			   + CMSG_SPACE(sizeof(int));

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_ALG;
	cmsg->cmsg_type  = ALG_SET_OP;
	cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
	p_op = (int *)CMSG_DATA(cmsg);
	*p_op = ALG_OP_ENCRYPT;

	cmsg = CMSG_NXTHDR(&msg, cmsg);
	cmsg->cmsg_level = SOL_ALG;
	cmsg->cmsg_type  = ALG_SET_IV;
	cmsg->cmsg_len   = CMSG_LEN(sizeof(*aiv) + IV_LEN);
	aiv = (struct af_alg_iv *)CMSG_DATA(cmsg);
	aiv->ivlen = IV_LEN;
	memcpy(aiv->iv, iv, IV_LEN);

	cmsg = CMSG_NXTHDR(&msg, cmsg);
	cmsg->cmsg_level = SOL_ALG;
	cmsg->cmsg_type  = ALG_SET_AEAD_ASSOCLEN;
	cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
	p_assoclen = (int *)CMSG_DATA(cmsg);
	*p_assoclen = AAD_LEN;

	/* AAD || plaintext */
	memset(buf, 0xaa, AAD_LEN);
	memset(buf + AAD_LEN, 0x55, PT_LEN);
	iov.iov_base = buf;
	iov.iov_len  = AAD_LEN + PT_LEN;
	msg.msg_iov    = &iov;
	msg.msg_iovlen = 1;

	n = sendmsg(opfd, &msg, 0);
	if (n < 0) {
		/*
		 * sendmsg refused: this is exactly the killswitch
		 * af_alg_sendmsg=-EPERM mitigation outcome.  Distinct
		 * exit code from setup failure so the test script can
		 * tell them apart.
		 */
		fprintf(stderr, "sendmsg: %s -- mitigation engaged?\n",
			strerror(errno));
		close(opfd); close(sk);
		return 1;
	}

	/* recv: AAD echoed, plus ciphertext + tag */
	memset(buf, 0, sizeof(buf));
	n = read(opfd, buf, EXPECTED_LEN);
	close(opfd); close(sk);

	if (n == 0) {
		printf("AEAD returned 0 bytes -- killswitch mitigation engaged\n");
		return 1;
	}
	if (n != EXPECTED_LEN) {
		fprintf(stderr,
			"AEAD short read: got %zd, expected %d -- mitigated?\n",
			n, EXPECTED_LEN);
		return 1;
	}

	/* sanity: ciphertext (after AAD) shouldn't equal the plaintext bytes */
	if (memcmp(buf + AAD_LEN, buf + AAD_LEN + 1, PT_LEN - 1) == 0) {
		fprintf(stderr, "AEAD output looks unencrypted\n");
		return 2;
	}

	printf("AEAD round-trip OK (%zd bytes)\n", n);
	return 0;
}
