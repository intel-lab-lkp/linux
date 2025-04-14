// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Rivos Inc.
 *
 * Authors:
 *     Clément Léger <cleger@rivosinc.com>
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/ptrace.h>
#include "../../kselftest_harness.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <float.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>
#include <inttypes.h>
#include <ucontext.h>

#include <sys/prctl.h>

#define stringify(s) __stringify(s)
#define __stringify(s) #s

#define VAL16	0x1234
#define VAL32	0xDEADBEEF
#define VAL64	0x45674321D00DF789

#define VAL_float	78951.234375
#define VAL_double	567890.512396965789589290

static bool float_equal(float a, float b)
{
	float scaled_epsilon;
	float difference = fabsf(a - b);

	// Scale to the largest value.
	a = fabsf(a);
	b = fabsf(b);
	if (a > b)
		scaled_epsilon = FLT_EPSILON * a;
	else
		scaled_epsilon = FLT_EPSILON * b;

	return difference <= scaled_epsilon;
}

static bool double_equal(double a, double b)
{
	double scaled_epsilon;
	double difference = fabsf(a - b);

	// Scale to the largest value.
	a = fabs(a);
	b = fabs(b);
	if (a > b)
		scaled_epsilon = DBL_EPSILON * a;
	else
		scaled_epsilon = DBL_EPSILON * b;

	return difference <= scaled_epsilon;
}

#define fpu_load_proto(__inst, __type) \
extern __type test_ ## __inst(unsigned long fp_reg, void *addr, unsigned long offset, __type value)

fpu_load_proto(flw, float);
fpu_load_proto(fld, double);
fpu_load_proto(c_flw, float);
fpu_load_proto(c_fld, double);
fpu_load_proto(c_fldsp, double);

#define fpu_store_proto(__inst, __type) \
extern void test_ ## __inst(unsigned long fp_reg, void *addr, unsigned long offset, __type value)

fpu_store_proto(fsw, float);
fpu_store_proto(fsd, double);
fpu_store_proto(c_fsw, float);
fpu_store_proto(c_fsd, double);
fpu_store_proto(c_fsdsp, double);

#define gp_load_proto(__inst, __type) \
extern __type test_ ## __inst(void *addr, unsigned long offset, __type value)

gp_load_proto(lh, uint16_t);
gp_load_proto(lhu, uint16_t);
gp_load_proto(lw, uint32_t);
gp_load_proto(lwu, uint32_t);
gp_load_proto(ld, uint64_t);
gp_load_proto(c_lw, uint32_t);
gp_load_proto(c_ld, uint64_t);
gp_load_proto(c_ldsp, uint64_t);

#define gp_store_proto(__inst, __type) \
extern void test_ ## __inst(void *addr, unsigned long offset, __type value)

gp_store_proto(sh, uint16_t);
gp_store_proto(sw, uint32_t);
gp_store_proto(sd, uint64_t);
gp_store_proto(c_sw, uint32_t);
gp_store_proto(c_sd, uint64_t);
gp_store_proto(c_sdsp, uint64_t);

#define TEST_GP_LOAD(__inst, __type_size)					\
TEST(gp_load_ ## __inst)							\
{										\
	int offset, ret;							\
	uint8_t buf[16] __attribute__((aligned(16)));				\
										\
	ret = prctl(PR_SET_UNALIGN, PR_UNALIGN_NOPRINT);			\
	ASSERT_EQ(ret, 0);							\
										\
	for (offset = 1; offset < __type_size / 8; offset++) {			\
		uint ## __type_size ## _t val = VAL ## __type_size;		\
		uint ## __type_size ## _t *ptr = (uint ## __type_size ## _t *) (buf + offset); \
		memcpy(ptr, &val, sizeof(val));					\
		val = test_ ## __inst(ptr, offset, val);			\
		EXPECT_EQ(VAL ## __type_size, val);				\
	}									\
}

TEST_GP_LOAD(lh, 16);
TEST_GP_LOAD(lhu, 16);
TEST_GP_LOAD(lw, 32);
TEST_GP_LOAD(lwu, 32);
TEST_GP_LOAD(ld, 64);
#ifdef __riscv_compressed
TEST_GP_LOAD(c_lw, 32);
TEST_GP_LOAD(c_ld, 64);
TEST_GP_LOAD(c_ldsp, 64);
#endif

#define TEST_GP_STORE(__inst, __type_size)					\
TEST(gp_load_ ## __inst)							\
{										\
	int offset, ret;							\
	uint8_t buf[16] __attribute__((aligned(16)));				\
										\
	ret = prctl(PR_SET_UNALIGN, PR_UNALIGN_NOPRINT);			\
	ASSERT_EQ(ret, 0);							\
										\
	for (offset = 1; offset < __type_size / 8; offset++) {			\
		uint ## __type_size ## _t val = VAL ## __type_size;		\
		uint ## __type_size ## _t *ptr = (uint ## __type_size ## _t *) (buf + offset); \
		memset(ptr, 0, sizeof(val));					\
		test_ ## __inst(ptr, offset, val);				\
		memcpy(&val, ptr, sizeof(val));					\
		EXPECT_EQ(VAL ## __type_size, val);				\
	}									\
}
TEST_GP_STORE(sh, 16);
TEST_GP_STORE(sw, 32);
TEST_GP_STORE(sd, 64);
#ifdef __riscv_compressed
TEST_GP_STORE(c_sw, 32);
TEST_GP_STORE(c_sd, 64);
TEST_GP_STORE(c_sdsp, 64);
#endif

#define __TEST_FPU_LOAD(__type, __inst, __reg_start, __reg_end)			\
TEST(fpu_load_ ## __inst)							\
{										\
	int i, ret, offset, fp_reg;						\
	uint8_t buf[16] __attribute__((aligned(16)));				\
										\
	ret = prctl(PR_SET_UNALIGN, PR_UNALIGN_NOPRINT);			\
	ASSERT_EQ(ret, 0);							\
										\
	for (fp_reg = __reg_start; fp_reg < __reg_end; fp_reg++) {		\
		for (offset = 1; offset < 4; offset++) {			\
			void *load_addr = (buf + offset);			\
			__type val = VAL_ ## __type ;				\
										\
			memcpy(load_addr, &val, sizeof(val));			\
			val = test_ ## __inst(fp_reg, load_addr, offset, val);	\
			EXPECT_TRUE(__type ##_equal(val, VAL_## __type));	\
		}								\
	}									\
}
#define TEST_FPU_LOAD(__type, __inst) \
	__TEST_FPU_LOAD(__type, __inst, 0, 32)
#define TEST_FPU_LOAD_COMPRESSED(__type, __inst) \
	__TEST_FPU_LOAD(__type, __inst, 8, 16)

TEST_FPU_LOAD(float, flw)
TEST_FPU_LOAD(double, fld)
#ifdef __riscv_compressed
TEST_FPU_LOAD_COMPRESSED(double, c_fld)
TEST_FPU_LOAD_COMPRESSED(double, c_fldsp)
#endif

#define __TEST_FPU_STORE(__type, __inst, __reg_start, __reg_end)		\
TEST(fpu_store_ ## __inst)							\
{										\
	int i, ret, offset, fp_reg;						\
	uint8_t buf[16] __attribute__((aligned(16)));				\
										\
	ret = prctl(PR_SET_UNALIGN, PR_UNALIGN_NOPRINT);			\
	ASSERT_EQ(ret, 0);							\
										\
	for (fp_reg = __reg_start; fp_reg < __reg_end; fp_reg++) {		\
		for (offset = 1; offset < 4; offset++) {			\
										\
			void *store_addr = (buf + offset);			\
			__type val = VAL_ ## __type ;				\
										\
			test_ ## __inst(fp_reg, store_addr, offset, val);	\
			memcpy(&val, store_addr, sizeof(val));			\
			EXPECT_TRUE(__type ## _equal(val, VAL_## __type));	\
		}								\
	}									\
}
#define TEST_FPU_STORE(__type, __inst) \
	__TEST_FPU_STORE(__type, __inst, 0, 32)
#define TEST_FPU_STORE_COMPRESSED(__type, __inst) \
	__TEST_FPU_STORE(__type, __inst, 8, 16)

TEST_FPU_STORE(float, fsw)
TEST_FPU_STORE(double, fsd)
#ifdef __riscv_compressed
TEST_FPU_STORE_COMPRESSED(double, c_fsd)
TEST_FPU_STORE_COMPRESSED(double, c_fsdsp)
#endif

TEST_SIGNAL(gen_sigbus, SIGBUS)
{
	uint32_t *ptr;
	uint8_t buf[16] __attribute__((aligned(16)));
	int ret;

	ret = prctl(PR_SET_UNALIGN, PR_UNALIGN_SIGBUS);
	ASSERT_EQ(ret, 0);

	ptr = (uint32_t *)(buf + 1);
	*ptr = 0xDEADBEEFULL;
}

int main(int argc, char **argv)
{
	int ret, val;

	ret = prctl(PR_GET_UNALIGN, &val);
	if (ret == -1 && errno == EINVAL)
		ksft_exit_skip("SKIP GET_UNALIGN_CTL not supported\n");

	exit(test_harness_run(argc, argv));
}
