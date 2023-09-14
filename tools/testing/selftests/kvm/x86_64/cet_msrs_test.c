// SPDX-License-Identifier: GPL-2.0
/*
 * Tests for CET control and data MSRs.
 */

#include <sys/ioctl.h>
#include <linux/bitmap.h>
#include "asm/msr-index.h"
#include "kvm_util.h"
#include "vmx.h"

#ifdef SELFTEST_DEBUG_MODE
#define CET_DEBUG_MODE
#endif

#define SET_MSR2_T(msr, msr_values, f1, f2)	\
{						\
	.idx = msr,				\
	.name = #msr,				\
	.valid = true,				\
	.has_f1 = (f1) ? true : false,		\
	.has_f2 = (f2) ? true : false,		\
	.nr_values = ARRAY_SIZE(msr_values),	\
	.values = msr_values			\
}

#define SET_MSR2_F(msr, msr_values, f1, f2)	\
{						\
	.idx = msr,				\
	.name = #msr,				\
	.valid = false,				\
	.has_f1 = (f1) ? true : false,		\
	.has_f2 = (f2) ? true : false,		\
	.nr_values = ARRAY_SIZE(msr_values),	\
	.values = msr_values			\
}

#define SET_MSR1_T(msr, msr_values, f)		\
	SET_MSR2_T(msr, msr_values, f, 0)

#define SET_MSR0_T(msr, msr_values)		\
	SET_MSR2_T(msr, msr_values, 0, 0)

#define CET_SHSTK_BITS (CET_SHSTK_EN | CET_WRSS_EN)

#define CET_IBT_BITS	(CET_LEG_IW_EN  | CET_NO_TRACK_EN |	\
			 CET_SUPPRESS_DISABLE | CET_SUPPRESS |	\
			 CET_WAIT_ENDBR)
#define CET_EB_LEG_BITMAP_BASE		(0xFFFFFFFFFFFFC000)
#define CET_EB_LEG_BITMAP_BASE_INVALID	(0xFFFFFFFFFFFFE000)
#define CET_SSP_BASE1			(0xFFFFFFFFFFFFFFFC)
#define CET_SSP_BASE2			(0xFFFFFFFFFFFFFFF8)
#define CET_SSP_BASE1_INVALID		(0xFFFFFFFFFFFFFFFE)
#define CET_SSP_BASE2_INVALID		(0xFFFFFFFFFFFFFFFF)
#define CET_SSP_BASE3_INVALID		(0x0FFFFFFFFFFFFFFF)

#define CET_SSP_TABLE_BASE1		(0xFFFFFFFFFFFFFFFC)
#define CET_SSP_TABLE_BASE2		(0xFFFFFFFFFFFFFFF8)
#define CET_SSP_TABLE_BASE3		(0xFFFFFFFFFFFFFFFE)
#define CET_SSP_TABLE_BASE4		(0xFFFFFFFFFFFFFFFF)
#define CET_SSP_TABLE_INVALID		(0x0FFFFFFFFFFFFFFF)

uint8_t cpu_law;

struct msr_data {
	const uint32_t idx;
	const char *name;
	const bool valid;
	const bool has_f1;
	const bool has_f2;
	uint32_t nr_values;
	const uint64_t *values;
};

static const uint64_t cet_ctrl_values[] = {
	CET_SHSTK_EN,
	CET_SHSTK_EN | CET_WRSS_EN,

	CET_ENDBR_EN | CET_NO_TRACK_EN,
	CET_ENDBR_EN | CET_SUPPRESS_DISABLE,
	CET_ENDBR_EN | CET_WAIT_ENDBR,
	CET_ENDBR_EN | CET_LEG_IW_EN,

	CET_ENDBR_EN | CET_NO_TRACK_EN | CET_SUPPRESS_DISABLE,
	CET_ENDBR_EN | CET_NO_TRACK_EN | CET_WAIT_ENDBR,
	CET_ENDBR_EN | CET_NO_TRACK_EN | CET_LEG_IW_EN,
	CET_ENDBR_EN | CET_SUPPRESS_DISABLE | CET_WAIT_ENDBR,
	CET_ENDBR_EN | CET_SUPPRESS_DISABLE | CET_LEG_IW_EN,
	CET_ENDBR_EN | CET_WAIT_ENDBR | CET_LEG_IW_EN,

	CET_ENDBR_EN | (CET_IBT_BITS & ~CET_SUPPRESS),

	CET_SHSTK_EN | CET_ENDBR_EN | CET_NO_TRACK_EN,
	CET_SHSTK_EN | CET_ENDBR_EN | CET_SUPPRESS_DISABLE,
	CET_SHSTK_EN | CET_ENDBR_EN | CET_WAIT_ENDBR,
	CET_SHSTK_EN | CET_ENDBR_EN | CET_LEG_IW_EN | CET_EB_LEG_BITMAP_BASE,

	CET_SHSTK_EN | CET_WRSS_EN | CET_ENDBR_EN | CET_NO_TRACK_EN,
	CET_SHSTK_EN | CET_WRSS_EN | CET_ENDBR_EN | CET_SUPPRESS_DISABLE,
	CET_SHSTK_EN | CET_WRSS_EN | CET_ENDBR_EN | CET_WAIT_ENDBR,
	CET_SHSTK_EN | CET_WRSS_EN | CET_ENDBR_EN | CET_LEG_IW_EN,

	CET_SHSTK_EN | CET_WRSS_EN | CET_ENDBR_EN | (CET_IBT_BITS & ~CET_SUPPRESS),
};

static const uint64_t cet_ctrl_invalid_values[] = {
	CET_INTEL_RESERVED,
	CET_SUPPRESS | CET_WAIT_ENDBR,
	CET_SHSTK_EN | CET_ENDBR_EN | CET_LEG_IW_EN | CET_EB_LEG_BITMAP_BASE_INVALID,
};

static const uint64_t cet_ssp_values[] = {
	CET_SSP_BASE1,
	CET_SSP_BASE2,
};

static const uint64_t cet_ssp_table_values[] = {
	CET_SSP_TABLE_BASE1,
	CET_SSP_TABLE_BASE2,
	CET_SSP_TABLE_BASE3,
	CET_SSP_TABLE_BASE4,
};

static const uint64_t cet_ssp_invalid_values[] = {
	CET_SSP_BASE1_INVALID,
	CET_SSP_BASE2_INVALID,
	CET_SSP_BASE3_INVALID
};

static const uint64_t cet_ssp_table_invalid_values[] = {
	CET_SSP_TABLE_INVALID
};

static const struct msr_data msr_cet_ctrl[] = {
	SET_MSR2_T(MSR_IA32_U_CET, cet_ctrl_values, 1, 1),
	SET_MSR2_T(MSR_IA32_U_CET, cet_ctrl_values, 1, 0),
	SET_MSR2_T(MSR_IA32_U_CET, cet_ctrl_values, 0, 1),
	SET_MSR2_T(MSR_IA32_U_CET, cet_ctrl_values, 0, 0),

	SET_MSR2_T(MSR_IA32_S_CET, cet_ctrl_values, 1, 1),
	SET_MSR2_T(MSR_IA32_S_CET, cet_ctrl_values, 1, 0),
	SET_MSR2_T(MSR_IA32_S_CET, cet_ctrl_values, 0, 1),
	SET_MSR2_T(MSR_IA32_S_CET, cet_ctrl_values, 0, 0),
};

static const struct msr_data msr_cet_ctrl_invalid[] = {
	SET_MSR2_F(MSR_IA32_U_CET, cet_ctrl_invalid_values, 1, 1),
	SET_MSR2_F(MSR_IA32_U_CET, cet_ctrl_invalid_values, 1, 0),
	SET_MSR2_F(MSR_IA32_U_CET, cet_ctrl_invalid_values, 0, 1),
	SET_MSR2_F(MSR_IA32_U_CET, cet_ctrl_invalid_values, 0, 0),

	SET_MSR2_F(MSR_IA32_S_CET, cet_ctrl_invalid_values, 1, 1),
	SET_MSR2_F(MSR_IA32_S_CET, cet_ctrl_invalid_values, 1, 0),
	SET_MSR2_F(MSR_IA32_S_CET, cet_ctrl_invalid_values, 0, 1),
	SET_MSR2_F(MSR_IA32_S_CET, cet_ctrl_invalid_values, 0, 0),
};

static const struct msr_data msr_cet_ssp[] = {
	SET_MSR2_T(MSR_IA32_PL0_SSP, cet_ssp_values, 1, 1),
	SET_MSR2_T(MSR_IA32_PL0_SSP, cet_ssp_values, 1, 0),
	SET_MSR2_T(MSR_IA32_PL0_SSP, cet_ssp_values, 0, 1),
	SET_MSR2_T(MSR_IA32_PL0_SSP, cet_ssp_values, 0, 0),

	SET_MSR2_T(MSR_IA32_PL1_SSP, cet_ssp_values, 1, 1),
	SET_MSR2_T(MSR_IA32_PL1_SSP, cet_ssp_values, 1, 0),
	SET_MSR2_T(MSR_IA32_PL1_SSP, cet_ssp_values, 0, 1),
	SET_MSR2_T(MSR_IA32_PL1_SSP, cet_ssp_values, 0, 0),

	SET_MSR2_T(MSR_IA32_PL2_SSP, cet_ssp_values, 1, 1),
	SET_MSR2_T(MSR_IA32_PL2_SSP, cet_ssp_values, 1, 0),
	SET_MSR2_T(MSR_IA32_PL2_SSP, cet_ssp_values, 0, 1),
	SET_MSR2_T(MSR_IA32_PL2_SSP, cet_ssp_values, 0, 0),

	SET_MSR2_T(MSR_IA32_PL3_SSP, cet_ssp_values, 1, 1),
	SET_MSR2_T(MSR_IA32_PL3_SSP, cet_ssp_values, 1, 0),
	SET_MSR2_T(MSR_IA32_PL3_SSP, cet_ssp_values, 0, 1),
	SET_MSR2_T(MSR_IA32_PL3_SSP, cet_ssp_values, 0, 0),
};

static const struct msr_data msr_cet_ssp_table[] = {
	SET_MSR2_T(MSR_IA32_INT_SSP_TAB, cet_ssp_table_values, 1, 1),
	SET_MSR2_T(MSR_IA32_INT_SSP_TAB, cet_ssp_table_values, 1, 0),
	SET_MSR2_T(MSR_IA32_INT_SSP_TAB, cet_ssp_table_values, 0, 1),
	SET_MSR2_T(MSR_IA32_INT_SSP_TAB, cet_ssp_table_values, 0, 0),
};

static const struct msr_data msr_cet_ssp_invalid[] = {
	SET_MSR2_F(MSR_IA32_PL0_SSP, cet_ssp_invalid_values, 1, 1),
	SET_MSR2_F(MSR_IA32_PL0_SSP, cet_ssp_invalid_values, 1, 0),
	SET_MSR2_F(MSR_IA32_PL0_SSP, cet_ssp_invalid_values, 0, 1),
	SET_MSR2_F(MSR_IA32_PL0_SSP, cet_ssp_invalid_values, 0, 0),

	SET_MSR2_F(MSR_IA32_PL1_SSP, cet_ssp_invalid_values, 1, 1),
	SET_MSR2_F(MSR_IA32_PL1_SSP, cet_ssp_invalid_values, 1, 0),
	SET_MSR2_F(MSR_IA32_PL1_SSP, cet_ssp_invalid_values, 0, 1),
	SET_MSR2_F(MSR_IA32_PL1_SSP, cet_ssp_invalid_values, 0, 0),

	SET_MSR2_F(MSR_IA32_PL2_SSP, cet_ssp_invalid_values, 1, 1),
	SET_MSR2_F(MSR_IA32_PL2_SSP, cet_ssp_invalid_values, 1, 0),
	SET_MSR2_F(MSR_IA32_PL2_SSP, cet_ssp_invalid_values, 0, 1),
	SET_MSR2_F(MSR_IA32_PL2_SSP, cet_ssp_invalid_values, 0, 0),

	SET_MSR2_F(MSR_IA32_PL3_SSP, cet_ssp_invalid_values, 1, 1),
	SET_MSR2_F(MSR_IA32_PL3_SSP, cet_ssp_invalid_values, 1, 0),
	SET_MSR2_F(MSR_IA32_PL3_SSP, cet_ssp_invalid_values, 0, 1),
	SET_MSR2_F(MSR_IA32_PL3_SSP, cet_ssp_invalid_values, 0, 0),
};

static const struct msr_data msr_cet_ssp_table_invalid[] = {
	SET_MSR2_F(MSR_IA32_INT_SSP_TAB, cet_ssp_table_invalid_values, 1, 1),
	SET_MSR2_F(MSR_IA32_INT_SSP_TAB, cet_ssp_table_invalid_values, 1, 0),
	SET_MSR2_F(MSR_IA32_INT_SSP_TAB, cet_ssp_table_invalid_values, 0, 1),
	SET_MSR2_F(MSR_IA32_INT_SSP_TAB, cet_ssp_table_invalid_values, 0, 0),
};

#ifdef CET_DEBUG_MODE
static inline char *get_cet_msr_string(uint32_t msr)
{
	return msr == MSR_IA32_U_CET ? "MSR_IA32_U_CET" :
		msr == MSR_IA32_S_CET ? "MSR_IA32_S_CET" :
		msr == MSR_IA32_PL3_SSP ? "MSR_IA32_PL3_SSP" :
		msr == MSR_IA32_PL2_SSP ? "MSR_IA32_PL2_SSP" :
		msr == MSR_IA32_PL1_SSP ? "MSR_IA32_PL1_SSP" :
		msr == MSR_IA32_PL0_SSP ? "MSR_IA32_PL0_SSP" :
		msr == MSR_IA32_INT_SSP_TAB ? "MSR_IA32_INT_SSP_TAB" :
		"Invalid CET msr!";
}
#endif

static inline int get_expected_result(uint32_t msr, uint64_t val,
				      bool has_shstk, bool has_ibt)
{
	int ret = 0;
	bool f1 = has_shstk, f2 = has_ibt;

	switch (msr) {
	case MSR_IA32_U_CET:
	case MSR_IA32_S_CET:
		ret = ((f1 && f2) || (f1 && !(CET_IBT_BITS & val)) ||
			(f2 && !(CET_SHSTK_BITS & val))) ? 1 : 0;
		break;
	case MSR_IA32_PL0_SSP:
	case MSR_IA32_PL1_SSP:
	case MSR_IA32_PL2_SSP:
	case MSR_IA32_PL3_SSP:
		ret = (f1 && is_canonical_addr(val, cpu_law) && !(val & 0x3)) ? 1 : 0;
		break;
	case MSR_IA32_INT_SSP_TAB:
		ret = f1 && is_canonical_addr(val, cpu_law) ? 1 : 0;
		break;
	default:
		break;
	}
	return ret;
}

static void cet_msr_test_runner(struct kvm_vcpu *vcpu, uint32_t msr,
				uint64_t val, bool f1, bool f2,
				bool valid)
{
	int nr_msr;
	uint64_t ret;

	if (f1)
		vcpu_set_cpuid_feature(vcpu, X86_FEATURE_SHSTK);
	else
		vcpu_clear_cpuid_feature(vcpu, X86_FEATURE_SHSTK);

	if (f2)
		vcpu_set_cpuid_feature(vcpu, X86_FEATURE_IBT);
	else
		vcpu_clear_cpuid_feature(vcpu, X86_FEATURE_IBT);

	nr_msr = get_expected_result(msr, val, f1, f2);

	if (!valid)
		nr_msr = 0;

	ret = _vcpu_set_msr(vcpu, msr, val);

	TEST_ASSERT(_vcpu_set_msr(vcpu, msr, val) == nr_msr,
		    "KVM_SET_MSRS should %s on 0x%x, value = 0x%lx",
		    nr_msr ? "succeed" : "fail", msr, val);

	if (!valid || !nr_msr)
		goto out;

	ret = vcpu_get_msr(vcpu, msr);

	TEST_ASSERT(ret == val,
		    "KVM_GET_MSRS returned different value = 0x%lx, origin = 0x%lx",
		    ret, val);
out:
#ifdef CET_DEBUG_MODE
	printf("Pass: %s %s with: %s && %s, msr val = 0x%lx\n",
	       valid && !!nr_msr ? "Write/Read" : "Write",
	       get_cet_msr_string(msr),
	       f1 ? "SHSTK" : "No SHSTK",
	       f2 ? "IBT" : "No IBT",
	       val);
#endif
}

static void run_cet_msr_tests(struct kvm_vcpu *vcpu,
			      struct msr_data const *msr_list,
			      uint32_t msr_list_size,
			      uint64_t const *msr_values_list,
			      uint32_t values_list_size)
{
	uint32_t msr;
	bool valid;
	bool shstk;
	bool ibt;
	int i, j;

	for (i = 0; i < msr_list_size; i++) {
		msr = msr_list[i].idx;
		shstk = msr_list[i].has_f1;
		ibt = msr_list[i].has_f2;
		valid = msr_list[i].valid;

		for (j = 0; j < values_list_size; j++)
			cet_msr_test_runner(vcpu, msr, msr_values_list[j],
					    shstk, ibt, valid);
	}
}

static void run_intel_cet_msr_tests(struct kvm_vcpu *vcpu)
{
	run_cet_msr_tests(vcpu, msr_cet_ctrl, ARRAY_SIZE(msr_cet_ctrl),
			  cet_ctrl_values, ARRAY_SIZE(cet_ctrl_values));

	run_cet_msr_tests(vcpu, msr_cet_ssp, ARRAY_SIZE(msr_cet_ssp),
			  cet_ssp_values, ARRAY_SIZE(cet_ssp_values));

	run_cet_msr_tests(vcpu, msr_cet_ssp_table,
			  ARRAY_SIZE(msr_cet_ssp_table),
			  cet_ssp_table_values,
			  ARRAY_SIZE(cet_ssp_table_values));

	run_cet_msr_tests(vcpu, msr_cet_ctrl_invalid,
			  ARRAY_SIZE(msr_cet_ctrl_invalid),
			  cet_ctrl_invalid_values,
			  ARRAY_SIZE(cet_ctrl_invalid_values));

	run_cet_msr_tests(vcpu, msr_cet_ssp_invalid,
			  ARRAY_SIZE(msr_cet_ssp_invalid),
			  cet_ssp_invalid_values,
			  ARRAY_SIZE(cet_ssp_invalid_values));

	run_cet_msr_tests(vcpu, msr_cet_ssp_table_invalid,
			  ARRAY_SIZE(msr_cet_ssp_table_invalid),
			  cet_ssp_table_invalid_values,
			  ARRAY_SIZE(cet_ssp_table_invalid_values));
}

static void run_amd_cet_msr_tests(struct kvm_vcpu *vcpu)
{
	/*
	 * TODO: After pass initial review, will add this part.
	 */
}

int main(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SHSTK));

	if (host_cpu_is_intel)
		TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_IBT));

	vm = vm_create_with_one_vcpu(&vcpu, NULL);

	cpu_law = this_cpu_has(X86_FEATURE_LA57) ? 57 : 48;

	if (host_cpu_is_intel)
		run_intel_cet_msr_tests(vcpu);
	else if (host_cpu_is_amd)
		run_amd_cet_msr_tests(vcpu);

	kvm_vm_free(vm);
}
