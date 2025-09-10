// SPDX-License-Identifier: GPL-2.0+
/* devmem test devmem.c
 *
 * Copyright (C) 2025 Red Hat, Inc. All Rights Reserved.
 * Written by Alessandro Carminati (acarmina@redhat.com)
 */

#define _GNU_SOURCE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utils.h"
#include "secret.h"
#include "debug.h"
#include "ram_map.h"
#include "tests.h"
#include "debug.h"
#include "../kselftest.h"

struct char_mem_test test_set[] = {
{
	"test_devmem_access",
	&test_devmem_access,
	"Test whether /dev/mem is accessible - memory_open FE_1, FE_2, FE_4",
	F_ARCH_ALL|F_BITS_ALL|F_MISC_FATAL|F_MISC_INIT_PRV
},
{	"test_open_devnum",
	&test_open_devnum,
	"Test open /dev/mem provides the correct min, maj - memory_open - FE_3",
	F_ARCH_ALL|F_BITS_ALL|F_MISC_INIT_REQ},
{
	"test_strict_devmem",
	&test_strict_devmem,
	"Test Strict Devmem enabled - Dependency",
	F_ARCH_ALL|F_BITS_ALL|F_MISC_STRICT_DEVMEM_PRV|F_MISC_DONT_CARE
},
{
	"test_read_at_addr_32bit_ge",
	&test_read_at_addr_32bit_ge,
	"Test read 64bit ppos vs 32 bit addr - read_mem - FE_1",
	F_ARCH_ALL|F_BITS_B32|F_MISC_INIT_REQ
},
{
	"test_read_outside_linear_map",
	&test_read_outside_linear_map,
	"Test read outside linear map - read_mem - FE_2",
	F_ARCH_ALL|F_BITS_B32|F_MISC_INIT_REQ
},
{
	"test_read_secret_area",
	&test_read_secret_area,
	"Test read memfd_secret area can not being accessed - read_mem - FE_4",
	F_ARCH_ALL|F_BITS_ALL|F_MISC_INIT_REQ
},
{
	"test_read_allowed_area",
	&test_read_allowed_area,
	"test read allowed area - read_mem - FE_5",
	F_ARCH_ALL|F_BITS_ALL|F_MISC_INIT_REQ
},
{
	"test_read_allowed_area_ppos_advance",
	&test_read_allowed_area_ppos_advance,
	"test read allowed area increments ppos - read_mem - FE_3",
	F_ARCH_ALL|F_BITS_ALL|F_MISC_INIT_REQ
},
{
	"test_read_restricted_area",
	&test_read_restricted_area,
	"test read restricted returns zeros - read_mem - FE_6",
	F_ARCH_X86|F_BITS_ALL|F_MISC_INIT_REQ|F_MISC_STRICT_DEVMEM_REQ
},
{
	"test_write_outside_area",
	&test_write_outside_area,
	"test write outside - write_mem - FE_2",
	F_ARCH_ALL|F_BITS_ALL|F_MISC_INIT_REQ|F_MISC_WARN_ON_FAILURE
},
{
	"test_seek_seek_set",
	&test_seek_seek_set,
	"test seek funcction SEEK_SET - memory_lseek - FE_4",
	F_ARCH_ALL|F_BITS_ALL|F_MISC_INIT_REQ
},
{
	"test_seek_seek_cur",
	&test_seek_seek_cur,
	"test seek function SEEK_CUR - memory_lseek - FE_3",
	F_ARCH_ALL|F_BITS_ALL|F_MISC_INIT_REQ
},
{
	"test_seek_seek_other",
	&test_seek_seek_other,
	"test seek function SEEK_END other - memory_lseek - FE_5",
	F_ARCH_ALL|F_BITS_ALL|F_MISC_INIT_REQ
},
};

int main(int argc, char *argv[])
{
	int tests_skipped = 0;
	int tests_failed = 0;
	int tests_passed = 0;
	int i, tmp_res;
	struct test_context t;
	char *str_res, *str_warn;
	struct char_mem_test *current;

	t.srcbuf = malloc_pb(BOUNCE_BUF_SIZE);
	t.dstbuf = malloc_pb(BOUNCE_BUF_SIZE);
	if (!t.srcbuf || !t.dstbuf) {
		printf("can't allocate buffers!\n");
		exit(-1);
	}
	// seet verbose flag from cmdline
	t.verbose = false;
	if ((argc >= 2) && (!strcmp(argv[1], "-v"))) {
		t.verbose = true;
		pdebug = 1;
	}

	t.map = parse_iomem();
	if (!t.map)
		goto exit;

	if (t.verbose) {
		report_physical_memory(t.map);
		dump_ram_map(t.map);
	}

	for (i = 0; i < ARRAY_SIZE(test_set); i++) {
		str_warn = NO_WARN_STR;
		current = test_set + i;
		tmp_res = test_needed(&t, current);
		switch (tmp_res) {
		case TEST_INCOHERENT:
			deb_printf("Incoherent sequence Detected\n");
			exit(-1);
			break;
		case TEST_ALLOWED:
			deb_printf("allowed sequence Detected\n");
			str_res = "";
			printf("%s - (%s) ", current->name, current->descr);
			tmp_res = current->fn(&t);
			switch (tmp_res) {
			case FAIL:
				str_res = DC_STR;
				if (!(current->flags & F_MISC_DONT_CARE)) {
					str_res = KO_STR;
					tests_failed++;
				}
				break;
			case SKIPPED:
				tests_skipped++;
				str_res = SKP_STR;
				if (current->flags & F_MISC_WARN_ON_FAILURE)
					str_warn = WARN_STR;
				break;
			case PASS:
				str_res = DC_STR;
				if (!(current->flags & F_MISC_DONT_CARE)) {
					tests_passed++;
					str_res = OK_STR;
				}
				if (current->flags & F_MISC_WARN_ON_SUCCESS)
					str_warn = WARN_STR;
				break;
			default:
				tests_failed++;
				printf("corrupted data\n");
				exit(-1);
			}
			ksft_print_msg("%s %s\n", str_res, str_warn);
			if ((tmp_res == FAIL) &&
			   (current->flags & F_MISC_FATAL)) {
				printf("fatal test failed end the chain\n");
				goto cleanup;
			}
		case TEST_DENIED:
			deb_printf("denied sequence Detected\n");
		}
	}

cleanup:
	close(t.fd);
	free_ram_map(t.map);
	free_pb(t.srcbuf);
	free_pb(t.dstbuf);
exit:
	printf("Run tests = %d (passed=%d, skipped=%d failed=%d)\n",
	    tests_skipped+tests_failed+tests_passed, tests_passed,
	    tests_skipped, tests_failed);
	return tests_skipped+tests_failed;
}
