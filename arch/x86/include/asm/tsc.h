/* SPDX-License-Identifier: GPL-2.0 */
/*
 * x86 TSC related functions
 */
#ifndef _ASM_X86_TSC_H
#define _ASM_X86_TSC_H

#include <asm/cpufeature.h>
#include <asm/processor.h>
#include <asm/msr.h>

/*
 * Standard way to access the cycle counter.
 */
typedef unsigned long long cycles_t;

extern unsigned int cpu_khz;
extern unsigned int tsc_khz;

extern void disable_TSC(void);

static inline cycles_t get_cycles(void)
{
	if (!IS_ENABLED(CONFIG_X86_TSC) &&
	    !cpu_feature_enabled(X86_FEATURE_TSC))
		return 0;
	return rdtsc();
}
#define get_cycles get_cycles

static inline int cpuid_get_tsc_info(unsigned int *crystal_khz,
				     unsigned int *denominator,
				     unsigned int *numerator)
{
	unsigned int ecx_hz, edx;

	if (boot_cpu_data.cpuid_level < CPUID_LEAF_TSC)
		return -ENOENT;

	*crystal_khz = *denominator = *numerator = ecx_hz = edx = 0;

	/* CPUID 15H TSC/Crystal ratio, plus optionally Crystal Hz */
	cpuid(CPUID_LEAF_TSC, denominator, numerator, &ecx_hz, &edx);

	if (!*denominator || !*numerator)
		return -ENOENT;

	/*
	 * Note, some CPUs provide the multiplier information, but not the core
	 * crystal frequency.  The multiplier information is still useful for
	 * such CPUs, as the crystal frequency can be gleaned from CPUID.0x16.
	 */
	*crystal_khz = ecx_hz / 1000;
	return 0;
}

static inline int cpuid_get_tsc_freq(unsigned int *tsc_khz,
				     unsigned int *crystal_khz)
{
	unsigned int denominator, numerator;

	if (cpuid_get_tsc_info(tsc_khz, &denominator, &numerator))
		return -ENOENT;

	if (!*crystal_khz)
		return -ENOENT;

	*tsc_khz = *crystal_khz * numerator / denominator;
	return 0;
}

static inline int cpuid_get_cpu_freq(unsigned int *cpu_khz)
{
	unsigned int eax_base_mhz, ebx, ecx, edx;

	if (boot_cpu_data.cpuid_level < CPUID_LEAF_FREQ)
		return -ENOENT;

	cpuid(CPUID_LEAF_FREQ, &eax_base_mhz, &ebx, &ecx, &edx);

	if (!eax_base_mhz)
		return -ENOENT;

	*cpu_khz = eax_base_mhz * 1000;
	return 0;
}

extern void tsc_early_init(void);
extern void tsc_init(void);
#if defined(CONFIG_HYPERVISOR_GUEST) || defined(CONFIG_AMD_MEM_ENCRYPT)
enum tsc_properties {
	TSC_FREQUENCY_KNOWN	= BIT(0),
	TSC_RELIABLE		= BIT(1),
	TSC_FREQ_KNOWN_AND_RELIABLE = TSC_FREQUENCY_KNOWN | TSC_RELIABLE,
};
extern void tsc_register_calibration_routines(unsigned long (*calibrate_tsc)(void),
					      unsigned long (*calibrate_cpu)(void),
					      enum tsc_properties properties);
#endif
extern void mark_tsc_unstable(char *reason);
extern int unsynchronized_tsc(void);
extern int check_tsc_unstable(void);
extern void mark_tsc_async_resets(char *reason);
extern unsigned long native_calibrate_cpu_early(void);
extern unsigned long native_calibrate_tsc(void);
extern unsigned long long native_sched_clock_from_tsc(u64 tsc);

extern int tsc_clocksource_reliable;
#ifdef CONFIG_X86_TSC
extern bool tsc_async_resets;
#else
# define tsc_async_resets	false
#endif

/*
 * Boot-time check whether the TSCs are synchronized across
 * all CPUs/cores:
 */
#ifdef CONFIG_X86_TSC
extern bool tsc_store_and_check_tsc_adjust(bool bootcpu);
extern void tsc_verify_tsc_adjust(bool resume);
extern void check_tsc_sync_target(void);
#else
static inline bool tsc_store_and_check_tsc_adjust(bool bootcpu) { return false; }
static inline void tsc_verify_tsc_adjust(bool resume) { }
static inline void check_tsc_sync_target(void) { }
#endif

extern int notsc_setup(char *);
extern void tsc_save_sched_clock_state(void);
extern void tsc_restore_sched_clock_state(void);

unsigned long cpu_khz_from_msr(void);

#endif /* _ASM_X86_TSC_H */
