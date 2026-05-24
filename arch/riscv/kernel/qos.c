// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/cpuhotplug.h>
#include <linux/notifier.h>
#include <linux/percpu-defs.h>
#include <linux/types.h>

#include <asm/cpufeature-macros.h>
#include <asm/hwcap.h>
#include <asm/qos.h>

/*
 * Cached value of srmcfg csr for each cpu. Seeded to U32_MAX so the next
 * __switch_to_srmcfg() unconditionally writes the CSR; the encoding
 * MCID << 16 | RCID with both fields well under 16 bits can never
 * produce this sentinel. This covers early-boot context switches that
 * happen before riscv_srmcfg_init() runs as an arch_initcall.
 */
DEFINE_PER_CPU(u32, cpu_srmcfg) = U32_MAX;

/* default srmcfg value for each cpu, set via resctrl cpu assignment */
DEFINE_PER_CPU(u32, cpu_srmcfg_default);

/*
 * Seed the per-CPU srmcfg cache to a sentinel that no real srmcfg encoding
 * can produce (MCID << 16 | RCID, both fields well under 16 bits) so the
 * next __switch_to_srmcfg() unconditionally writes the CSR. Ssqosid v1.0
 * leaves CSR state across hart stop/start implementation-defined, so the
 * cached value cannot be trusted after online.
 */
static int riscv_srmcfg_online(unsigned int cpu)
{
	per_cpu(cpu_srmcfg, cpu) = U32_MAX;
	return 0;
}

/*
 * CPU PM notifier: invalidate the cached srmcfg on resume from a deep
 * idle / suspend. Ssqosid v1.0 leaves CSR_SRMCFG state across low-power
 * transitions implementation-defined, and the boot CPU never goes
 * through the cpuhp online callback during system suspend, so without
 * this hook __switch_to_srmcfg() would skip the CSR write when the
 * outgoing task happens to share its srmcfg with the pre-suspend cache.
 */
static int riscv_srmcfg_pm_notify(struct notifier_block *nb,
				  unsigned long action, void *unused)
{
	switch (action) {
	case CPU_PM_EXIT:
	case CPU_PM_ENTER_FAILED:
		__this_cpu_write(cpu_srmcfg, U32_MAX);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block riscv_srmcfg_pm_nb = {
	.notifier_call = riscv_srmcfg_pm_notify,
};

static int __init riscv_srmcfg_init(void)
{
	int err;

	if (!riscv_has_extension_unlikely(RISCV_ISA_EXT_SSQOSID))
		return 0;

	/*
	 * cpuhp_setup_state() invokes the startup callback locally on every
	 * already-online CPU, so no separate seed loop is needed here.
	 */
	err = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "riscv/srmcfg:online",
				riscv_srmcfg_online, NULL);
	if (err < 0) {
		pr_warn("srmcfg cpuhp registration failed (%d), cpus brought online after boot will not invalidate the CSR_SRMCFG cache\n",
			err);
		return err;
	}

	cpu_pm_register_notifier(&riscv_srmcfg_pm_nb);
	return 0;
}
arch_initcall(riscv_srmcfg_init);
