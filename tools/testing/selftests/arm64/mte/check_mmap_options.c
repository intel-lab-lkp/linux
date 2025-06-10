// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2020 ARM Limited

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "kselftest.h"
#include "mte_common_util.h"
#include "mte_def.h"

#define RUNS			(MT_TAG_COUNT)
#define UNDERFLOW		MT_GRANULE_SIZE
#define OVERFLOW		MT_GRANULE_SIZE
#define TAG_CHECK_ON		0
#define TAG_CHECK_OFF		1

#define TEST_NAME_MAX		256

#define CHECK_ANON_MEM		0
#define CHECK_FILE_MEM		1
#define CHECK_CLEAR_PROT_MTE	2

#define ATAG_TEST_ON		1
#define ATAG_TEST_OFF		0

static size_t page_size;
static int sizes[] = {
	1, 537, 989, 1269, MT_GRANULE_SIZE - 1, MT_GRANULE_SIZE,
	/* page size - 1*/ 0, /* page_size */ 0, /* page size + 1 */ 0
};

static int check_mte_memory(char *ptr, int size, int mode, int tag_check, int atag_test)
{
	int err;

	if (!mtefar_support && atag_test == ATAG_TEST_ON)
		return KSFT_SKIP;

	if (atag_test == ATAG_TEST_ON)
		ptr = mte_insert_atag(ptr);

	mte_initialize_current_context(mode, (uintptr_t)ptr, size);
	memset(ptr, '1', size);
	mte_wait_after_trig();
	if (cur_mte_cxt.fault_valid == true)
		return KSFT_FAIL;

	mte_initialize_current_context(mode, (uintptr_t)ptr, -UNDERFLOW);
	memset(ptr - UNDERFLOW, '2', UNDERFLOW);
	mte_wait_after_trig();
	if (cur_mte_cxt.fault_valid == false && tag_check == TAG_CHECK_ON)
		return KSFT_FAIL;
	if (cur_mte_cxt.fault_valid == true && tag_check == TAG_CHECK_OFF)
		return KSFT_FAIL;

	mte_initialize_current_context(mode, (uintptr_t)ptr, size + OVERFLOW);
	memset(ptr + size, '3', OVERFLOW);
	mte_wait_after_trig();
	if (cur_mte_cxt.fault_valid == false && tag_check == TAG_CHECK_ON)
		return KSFT_FAIL;
	if (cur_mte_cxt.fault_valid == true && tag_check == TAG_CHECK_OFF)
		return KSFT_FAIL;

	return KSFT_PASS;
}

static int check_anonymous_memory_mapping(int mem_type, int mode, int mapping, int tag_check, int atag_test)
{
	char *ptr, *map_ptr;
	int run, result, map_size;
	int item = ARRAY_SIZE(sizes);

	mte_switch_mode(mode, MTE_ALLOW_NON_ZERO_TAG);
	for (run = 0; run < item; run++) {
		map_size = sizes[run] + OVERFLOW + UNDERFLOW;
		map_ptr = (char *)mte_allocate_memory(map_size, mem_type, mapping, false);
		if (check_allocated_memory(map_ptr, map_size, mem_type, false) != KSFT_PASS)
			return KSFT_FAIL;

		ptr = map_ptr + UNDERFLOW;
		mte_initialize_current_context(mode, (uintptr_t)ptr, sizes[run]);
		/* Only mte enabled memory will allow tag insertion */
		ptr = mte_insert_tags((void *)ptr, sizes[run]);
		if (!ptr || cur_mte_cxt.fault_valid == true) {
			ksft_print_msg("FAIL: Insert tags on anonymous mmap memory\n");
			munmap((void *)map_ptr, map_size);
			return KSFT_FAIL;
		}
		result = check_mte_memory(ptr, sizes[run], mode, tag_check, atag_test);
		mte_clear_tags((void *)ptr, sizes[run]);
		mte_free_memory((void *)map_ptr, map_size, mem_type, false);
		if (result != KSFT_SKIP)
			return result;
	}
	return KSFT_PASS;
}

static int check_file_memory_mapping(int mem_type, int mode, int mapping, int tag_check, int atag_test)
{
	char *ptr, *map_ptr;
	int run, fd, map_size;
	int total = ARRAY_SIZE(sizes);
	int result = KSFT_PASS;

	mte_switch_mode(mode, MTE_ALLOW_NON_ZERO_TAG);
	for (run = 0; run < total; run++) {
		fd = create_temp_file();
		if (fd == -1)
			return KSFT_FAIL;

		map_size = sizes[run] + UNDERFLOW + OVERFLOW;
		map_ptr = (char *)mte_allocate_file_memory(map_size, mem_type, mapping, false, fd);
		if (check_allocated_memory(map_ptr, map_size, mem_type, false) != KSFT_PASS) {
			close(fd);
			return KSFT_FAIL;
		}
		ptr = map_ptr + UNDERFLOW;
		mte_initialize_current_context(mode, (uintptr_t)ptr, sizes[run]);
		/* Only mte enabled memory will allow tag insertion */
		ptr = mte_insert_tags((void *)ptr, sizes[run]);
		if (!ptr || cur_mte_cxt.fault_valid == true) {
			ksft_print_msg("FAIL: Insert tags on file based memory\n");
			munmap((void *)map_ptr, map_size);
			close(fd);
			return KSFT_FAIL;
		}
		result = check_mte_memory(ptr, sizes[run], mode, tag_check, atag_test);
		mte_clear_tags((void *)ptr, sizes[run]);
		munmap((void *)map_ptr, map_size);
		close(fd);
		if (result == KSFT_FAIL)
			break;
	}
	return result;
}

static int check_clear_prot_mte_flag(int mem_type, int mode, int mapping, int atag_test)
{
	char *ptr, *map_ptr;
	int run, prot_flag, result, fd, map_size;
	int total = ARRAY_SIZE(sizes);

	prot_flag = PROT_READ | PROT_WRITE;
	mte_switch_mode(mode, MTE_ALLOW_NON_ZERO_TAG);
	for (run = 0; run < total; run++) {
		map_size = sizes[run] + OVERFLOW + UNDERFLOW;
		ptr = (char *)mte_allocate_memory_tag_range(sizes[run], mem_type, mapping,
							    UNDERFLOW, OVERFLOW);
		if (check_allocated_memory_range(ptr, sizes[run], mem_type,
						 UNDERFLOW, OVERFLOW) != KSFT_PASS)
			return KSFT_FAIL;
		map_ptr = ptr - UNDERFLOW;
		/* Try to clear PROT_MTE property and verify it by tag checking */
		if (mprotect(map_ptr, map_size, prot_flag)) {
			mte_free_memory_tag_range((void *)ptr, sizes[run], mem_type,
						  UNDERFLOW, OVERFLOW);
			ksft_print_msg("FAIL: mprotect not ignoring clear PROT_MTE property\n");
			return KSFT_FAIL;
		}
		result = check_mte_memory(ptr, sizes[run], mode, TAG_CHECK_ON, atag_test);
		mte_free_memory_tag_range((void *)ptr, sizes[run], mem_type, UNDERFLOW, OVERFLOW);
		if (result != KSFT_PASS)
			return result;

		fd = create_temp_file();
		if (fd == -1)
			return KSFT_FAIL;
		ptr = (char *)mte_allocate_file_memory_tag_range(sizes[run], mem_type, mapping,
								 UNDERFLOW, OVERFLOW, fd);
		if (check_allocated_memory_range(ptr, sizes[run], mem_type,
						 UNDERFLOW, OVERFLOW) != KSFT_PASS) {
			close(fd);
			return KSFT_FAIL;
		}
		map_ptr = ptr - UNDERFLOW;
		/* Try to clear PROT_MTE property and verify it by tag checking */
		if (mprotect(map_ptr, map_size, prot_flag)) {
			ksft_print_msg("FAIL: mprotect not ignoring clear PROT_MTE property\n");
			mte_free_memory_tag_range((void *)ptr, sizes[run], mem_type,
						  UNDERFLOW, OVERFLOW);
			close(fd);
			return KSFT_FAIL;
		}
		result = check_mte_memory(ptr, sizes[run], mode, TAG_CHECK_ON, atag_test);
		mte_free_memory_tag_range((void *)ptr, sizes[run], mem_type, UNDERFLOW, OVERFLOW);
		close(fd);
		if (result != KSFT_PASS)
			return result;
	}
	return KSFT_PASS;
}

const char *format_test_name(int check_type, int mem_type, int sync,
		       int mapping, int tag_check, int atag_test)
{
	static char test_name[TEST_NAME_MAX];
	const char* check_type_str;
	const char* mem_type_str;
	const char* sync_str;
	const char* mapping_str;
	const char* tag_check_str;
	const char *atag_test_str;

	switch (check_type) {
	case CHECK_ANON_MEM:
		check_type_str = "anonymous memory";
		break;
	case CHECK_FILE_MEM:
		check_type_str = "file memory";
		break;
	case CHECK_CLEAR_PROT_MTE:
		check_type_str = "clear PROT_MTE flags";
		break;
	default:
		assert(0);
		break;
	}

	switch (mem_type) {
	case USE_MMAP:
		mem_type_str = "mmap";
		break;
	case USE_MPROTECT:
		mem_type_str = "mmap/mprotect";
		break;
	default:
		assert(0);
		break;
	}

	switch (sync) {
	case MTE_NONE_ERR:
		sync_str = "no error";
		break;
	case MTE_SYNC_ERR:
		sync_str = "sync error";
		break;
	case MTE_ASYNC_ERR:
		sync_str = "async error";
		break;
	default:
		assert(0);
		break;
	}

	switch (mapping) {
	case MAP_SHARED:
		mapping_str = "shared";
		break;
	case MAP_PRIVATE:
		mapping_str = "private";
		break;
	default:
		assert(0);
		break;
	}

	switch (tag_check) {
	case TAG_CHECK_ON:
		tag_check_str = "tag check on";
		break;
	case TAG_CHECK_OFF:
		tag_check_str = "tag check off";
		break;
	default:
		assert(0);
		break;
	}

	switch (atag_test) {
	case ATAG_TEST_ON:
		atag_test_str = "with address tag [63:60]";
		break;
	case ATAG_TEST_OFF:
		atag_test_str = "without address tag [63:60]";
		break;
	default:
		assert(0);
		break;
	}

	snprintf(test_name, TEST_NAME_MAX,
	         "Check %s with %s mapping, %s mode, %s memory and %s (%s)\n",
	         check_type_str, mapping_str, sync_str, mem_type_str,
	         tag_check_str, atag_test_str);

	return test_name;
}

int main(int argc, char *argv[])
{
	int err;
	int item = ARRAY_SIZE(sizes);
	int check_type[] = { CHECK_ANON_MEM, CHECK_FILE_MEM };
	int mem_type[] = { USE_MMAP, USE_MPROTECT };
	int mte_sync[] = { MTE_SYNC_ERR, MTE_ASYNC_ERR };
	int mapping[] = { MAP_PRIVATE, MAP_SHARED };
	int atag_test[] = { ATAG_TEST_OFF, ATAG_TEST_ON };
	int c, mt, s, m, a;

	err = mte_default_setup();
	if (err)
		return err;
	page_size = getpagesize();
	if (!page_size) {
		ksft_print_msg("ERR: Unable to get page size\n");
		return KSFT_FAIL;
	}
	sizes[item - 3] = page_size - 1;
	sizes[item - 2] = page_size;
	sizes[item - 1] = page_size + 1;

	/* Set test plan */
	ksft_set_plan(44);

	for (a = 0; a < ARRAY_SIZE(atag_test); a++) {
		/* Register signal handlers */
		mte_register_signal(SIGBUS, mte_default_handler, atag_test[a]);
		mte_register_signal(SIGSEGV, mte_default_handler, atag_test[a]);

		mte_enable_pstate_tco();

		evaluate_test(check_anonymous_memory_mapping(USE_MMAP, MTE_SYNC_ERR, MAP_PRIVATE, TAG_CHECK_OFF, atag_test[a]),
			      format_test_name(CHECK_ANON_MEM, USE_MMAP, MTE_SYNC_ERR, MAP_PRIVATE, TAG_CHECK_OFF, atag_test[a]));

		evaluate_test(check_file_memory_mapping(USE_MPROTECT, MTE_SYNC_ERR, MAP_PRIVATE, TAG_CHECK_OFF, atag_test[a]),
			      format_test_name(CHECK_FILE_MEM, USE_MPROTECT, MTE_SYNC_ERR, MAP_PRIVATE, TAG_CHECK_OFF, atag_test[a]));

		mte_disable_pstate_tco();

		evaluate_test(check_anonymous_memory_mapping(USE_MMAP, MTE_NONE_ERR, MAP_PRIVATE, TAG_CHECK_OFF, atag_test[a]),
			      format_test_name(CHECK_ANON_MEM, USE_MMAP, MTE_NONE_ERR, MAP_PRIVATE, TAG_CHECK_OFF, atag_test[a]));
		evaluate_test(check_file_memory_mapping(USE_MPROTECT, MTE_NONE_ERR, MAP_PRIVATE, TAG_CHECK_OFF, atag_test[a]),
			      format_test_name(CHECK_FILE_MEM, USE_MPROTECT, MTE_NONE_ERR, MAP_PRIVATE, TAG_CHECK_OFF, atag_test[a]));

		for (c = 0 ; c < ARRAY_SIZE(check_type); c++) {
			for (s = 0; s < ARRAY_SIZE(mte_sync); s++) {
				for (m = 0; m < ARRAY_SIZE(mapping); m++) {
					for (mt = 0; mt < ARRAY_SIZE(mem_type); mt++) {
						if (check_type[c] == CHECK_ANON_MEM)
							evaluate_test(check_anonymous_memory_mapping(mem_type[mt], mte_sync[s], mapping[m], TAG_CHECK_ON, atag_test[a]),
								format_test_name(CHECK_ANON_MEM, mem_type[mt], mte_sync[s], mapping[m], TAG_CHECK_ON, atag_test[a]));
						else
							evaluate_test(check_file_memory_mapping(mem_type[mt], mte_sync[s], mapping[m], TAG_CHECK_ON, atag_test[a]),
								format_test_name(CHECK_FILE_MEM, mem_type[mt], mte_sync[s], mapping[m], TAG_CHECK_ON, atag_test[a]));
					}
				}
			}
		}

		evaluate_test(check_clear_prot_mte_flag(USE_MMAP, MTE_SYNC_ERR, MAP_PRIVATE, atag_test[a]),
			      format_test_name(CHECK_CLEAR_PROT_MTE, USE_MMAP, MTE_SYNC_ERR, MAP_PRIVATE, TAG_CHECK_ON, atag_test[a]));
		evaluate_test(check_clear_prot_mte_flag(USE_MPROTECT, MTE_SYNC_ERR, MAP_PRIVATE, atag_test[a]),
			      format_test_name(CHECK_CLEAR_PROT_MTE, USE_MPROTECT, MTE_SYNC_ERR, MAP_PRIVATE, TAG_CHECK_ON, atag_test[a]));
	}

	mte_restore_setup();
	ksft_print_cnts();
	return ksft_get_fail_cnt() == 0 ? KSFT_PASS : KSFT_FAIL;
}
