/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_MICROCODE_H
#define _ASM_X86_MICROCODE_H

#include <asm/cpu.h>
#include <asm/intel-family.h>
#include <asm/msr.h>

struct cpu_signature {
	unsigned int sig;
	unsigned int pf;
	unsigned int rev;
};

struct ucode_cpu_info {
	struct cpu_signature	cpu_sig;
	void			*mc;
};

#ifdef CONFIG_MICROCODE
void load_ucode_bsp(void);
void load_ucode_ap(void);
void microcode_bsp_resume(void);
bool __init microcode_loader_disabled(void);
#else
static inline void load_ucode_bsp(void)	{ }
static inline void load_ucode_ap(void) { }
static inline void microcode_bsp_resume(void) { }
static inline bool __init microcode_loader_disabled(void) { return false; }
#endif

extern unsigned long initrd_start_early;

#ifdef CONFIG_CPU_SUP_INTEL
/* Intel specific microcode defines. Public for IFS */
struct microcode_header_intel {
	unsigned int	hdrver;
	unsigned int	rev;
	unsigned int	date;
	unsigned int	sig;
	unsigned int	cksum;
	unsigned int	ldrver;
	unsigned int	pf;
	unsigned int	datasize;
	unsigned int	totalsize;
	unsigned int	metasize;
	unsigned int	min_req_ver;
	unsigned int	reserved;
};

struct microcode_intel {
	struct microcode_header_intel	hdr;
	unsigned int			bits[];
};

#define DEFAULT_UCODE_DATASIZE		(2000)
#define MC_HEADER_SIZE			(sizeof(struct microcode_header_intel))
#define MC_HEADER_TYPE_MICROCODE	1
#define MC_HEADER_TYPE_IFS		2

static inline int intel_microcode_get_datasize(struct microcode_header_intel *hdr)
{
	return hdr->datasize ? : DEFAULT_UCODE_DATASIZE;
}

static inline u32 intel_get_microcode_revision(void)
{
	u32 rev, dummy;

	native_wrmsrq(MSR_IA32_UCODE_REV, 0);

	/* As documented in the SDM: Do a CPUID 1 here */
	native_cpuid_eax(1);

	/* get the current revision from MSR 0x8B */
	native_rdmsr(MSR_IA32_UCODE_REV, dummy, rev);

	return rev;
}

/*
 * Use CPUID to generate a "vfm" value. Useful
 * before 'cpuinfo_x86' structures are populated.
 */
static inline u32 intel_cpuid_vfm(void)
{
	u32 eax   = cpuid_eax(1);
	u32 fam   = x86_family(eax);
	u32 model = x86_model(eax);

	return IFM(fam, model);
}

static inline u32 intel_get_platform_id(void)
{
	unsigned int val[2];

	/*
	 * This can be called early. Use CPUID directly to
	 * generate the VFM value for this CPU.
	 */
	if (intel_cpuid_vfm() < INTEL_PENTIUM_III_DESCHUTES)
		return 0;

	/* get processor flags from MSR 0x17 */
	native_rdmsr(MSR_IA32_PLATFORM_ID, val[0], val[1]);

	return (val[1] >> 18) & 7;
}
#endif /* !CONFIG_CPU_SUP_INTEL */

bool microcode_nmi_handler(void);
void microcode_offline_nmi_handler(void);

#ifdef CONFIG_MICROCODE_LATE_LOADING
DECLARE_STATIC_KEY_FALSE(microcode_nmi_handler_enable);
static __always_inline bool microcode_nmi_handler_enabled(void)
{
	return static_branch_unlikely(&microcode_nmi_handler_enable);
}
#else
static __always_inline bool microcode_nmi_handler_enabled(void) { return false; }
#endif

#endif /* _ASM_X86_MICROCODE_H */
