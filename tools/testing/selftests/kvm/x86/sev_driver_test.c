// SPDX-License-Identifier: GPL-2.0-only
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <math.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "svm_util.h"
#include "linux/psp-sev.h"
#include "sev.h"

static u8 buffer[PAGE_SIZE * 4];
static u8 buffer2[PAGE_SIZE * 4];
static int sev_fd;

#define SEV_INVALID_LENGTH	0x4

static int sev_driver_do_cmd(u32 cmd, void *data)
{
	struct sev_issue_cmd issue_cmd = {
		.cmd = cmd,
		.data = (u64)(unsigned long)data,
	};

	int r = __kvm_ioctl(sev_fd, SEV_ISSUE_CMD, &issue_cmd);

	TEST_ASSERT(!r || issue_cmd.error,
		    "Kernel returned r=%d, errno=%d without a firmware error", r, errno);
	return r ? issue_cmd.error : 0;
}

static void sev_driver_get_lengths(u32 cmd, void *data)
{
	int r;

	r = sev_driver_do_cmd(cmd, data);
	TEST_ASSERT_EQ(r, SEV_INVALID_LENGTH);
}

static void __sev_test_lengths(u32 cmd, const char *str, void *data,
			       u32 *len1, u32 *len2)
{
	u32 i, min1, min2;
	int r;

	min1 = *len1;
	min2 = len2 ? *len2 : 0;

	TEST_ASSERT(min1 >= 8 && min1 <= sizeof(buffer),
		    "Wanted length >= 8, <= 0x%x, got length = 0x%x",
		    (u32)sizeof(buffer), min1);

	TEST_ASSERT(!len2 || (min2 >= 8 && min2 <= sizeof(buffer)),
		    "Wanted length >= 8, <= 0x%x, got length = 0x%x",
		    (u32)sizeof(buffer), min2);

	for (i = 0; i < min1; i++) {
		*len1 = i;

		r = sev_driver_do_cmd(cmd, data);
		TEST_ASSERT_EQ(r, SEV_INVALID_LENGTH);
	}

	for (i = min1; i < min(min1 + 8, (u32)sizeof(buffer)); i++) {
		*len1 = i;
		r = sev_driver_do_cmd(cmd, data);
		TEST_ASSERT(!r, __KVM_SYSCALL_ERROR(str, r));
	}

	if (!len2)
		goto skip_len2;

	for (i = 0; i < min2; i++) {
		*len2 = i;

		r = sev_driver_do_cmd(cmd, data);
		/* Apparently firmware truncates its output for PDH_CERT_EXPORT? */
		TEST_ASSERT(r == SEV_INVALID_LENGTH || (!r && *len2 == i),
			    "Wanted INVALID_LENGTH or same length, got r=%u, len=0x%x (from 0x%x)",
			    r, *len2, i);
	}

	for (i = min2; i < min(min2 + 8, (u32)sizeof(buffer2)); i++) {
		*len2 = i;
		r = sev_driver_do_cmd(cmd, data);
		TEST_ASSERT(!r, __KVM_SYSCALL_ERROR(str, r));
	}

	*len2 = sizeof(buffer2);
skip_len2:
	*len1 = sizeof(buffer);
	r = sev_driver_do_cmd(cmd, data);
	TEST_ASSERT(!r, __KVM_SYSCALL_ERROR(str, r));
}

#define sev_test_lengths(__cmd, __data, __len1, __len2)	\
	__sev_test_lengths(__cmd, #__cmd, __data, __len1, __len2)

static void sev_test_pek_csr(void)
{
	struct sev_user_data_pek_csr pek_csr = {
		.address = (u64)buffer,
	};

	sev_driver_get_lengths(SEV_PEK_CSR, &pek_csr);
	sev_test_lengths(SEV_PEK_CSR, &pek_csr, &pek_csr.length, NULL);
}

static void sev_test_pdh_cert(void)
{
	struct sev_user_data_pdh_cert_export cert = {
		.pdh_cert_address = (u64)buffer,
		.cert_chain_address = (u64)(buffer2),
	};

	sev_driver_get_lengths(SEV_PDH_CERT_EXPORT, &cert);
	sev_test_lengths(SEV_PDH_CERT_EXPORT, &cert, &cert.pdh_cert_len, &cert.cert_chain_len);
}

static void sev_test_get_id2(void)
{
	struct sev_user_data_get_id2 id2 = {
		.address = (u64)buffer,
	};

	sev_driver_get_lengths(SEV_GET_ID2, &id2);
	sev_test_lengths(SEV_GET_ID2, &id2, &id2.length, NULL);
}

int main(int argc, char *argv[])
{
	sev_fd = open_path_or_exit("/dev/sev", O_RDWR);

	sev_test_pek_csr();
	sev_test_pdh_cert();
	sev_test_get_id2();

	return 0;
}
