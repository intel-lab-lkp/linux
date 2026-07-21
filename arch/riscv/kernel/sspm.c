// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cpu.h>
#include <linux/mutex.h>
#include <linux/panic.h>
#include <linux/printk.h>
#include <linux/smp.h>

#include <asm/sbi.h>
#include <asm/sspm.h>

#define RISCV_SSPM_PMLEN	7

enum riscv_sspm_state {
	RISCV_SSPM_DISABLED,
	RISCV_SSPM_ENABLING,
	RISCV_SSPM_ENABLED,
	RISCV_SSPM_FAILED,
	RISCV_SSPM_BROKEN,
};

static enum riscv_sspm_state riscv_sspm_state __read_mostly;
static bool riscv_sspm_smp_ready __read_mostly = !IS_ENABLED(CONFIG_SMP);
#ifdef CONFIG_RISCV_ISA_SSPM
static DEFINE_MUTEX(riscv_sspm_lock);
static int riscv_sspm_error __read_mostly;
#endif

static int riscv_sspm_set_local(unsigned long pmlen)
{
	unsigned long value;
	int ret;

	ret = sbi_fwft_set(SBI_FWFT_POINTER_MASKING_PMLEN, pmlen, 0);
	if (ret)
		return ret;

	ret = sbi_fwft_get(SBI_FWFT_POINTER_MASKING_PMLEN, &value);
	if (ret)
		return ret;

	return value == pmlen ? 0 : -EIO;
}

static int riscv_sspm_reset_local(void)
{
	int ret;

	ret = riscv_sspm_set_local(0);
	return ret == -EOPNOTSUPP ? 0 : ret;
}

void riscv_sspm_reset_local_or_panic(const char *context)
{
	int ret;

	ret = riscv_sspm_reset_local();
	if (ret)
		panic("Sspm: failed to reset %s PMLEN: %d", context, ret);
}

void __init riscv_sspm_boot_reset(void)
{
	/*
	 * Firmware may retain FWFT state across kexec or a crash handoff. Reset
	 * it before the Linux successor starts relying on canonical addresses.
	 */
	riscv_sspm_reset_local_or_panic("boot hart");
}

unsigned long riscv_sspm_hibernate_pmlen(void)
{
	return riscv_sspm_enabled() ? RISCV_SSPM_PMLEN : 0;
}

bool riscv_sspm_hibernate_pmlen_valid(unsigned long pmlen)
{
	return pmlen == 0 || pmlen == RISCV_SSPM_PMLEN;
}

int riscv_sspm_hibernate_restore(unsigned long pmlen)
{
	int ret;

	if (!riscv_sspm_hibernate_pmlen_valid(pmlen))
		return -EINVAL;

	if (!pmlen) {
		riscv_sspm_reset_local_or_panic("hibernate");
		return 0;
	}

	ret = riscv_sspm_set_local(pmlen);
	if (ret)
		riscv_sspm_reset_local_or_panic("hibernate cleanup");

	return ret;
}

void __init riscv_sspm_smp_cpus_done(void)
{
	WRITE_ONCE(riscv_sspm_smp_ready, true);
}

bool riscv_sspm_may_be_active(void)
{
	enum riscv_sspm_state state = READ_ONCE(riscv_sspm_state);

	return state == RISCV_SSPM_ENABLING || state == RISCV_SSPM_ENABLED ||
	       state == RISCV_SSPM_BROKEN;
}

int riscv_sspm_prepare_cpu(void)
{
	enum riscv_sspm_state state = READ_ONCE(riscv_sspm_state);
	unsigned int cpu = smp_processor_id();
	int ret, cleanup_ret;

	if (state == RISCV_SSPM_DISABLED || state == RISCV_SSPM_FAILED ||
	    state == RISCV_SSPM_BROKEN) {
		ret = riscv_sspm_reset_local();
		if (ret)
			pr_crit("Sspm: CPU%u failed to reset PMLEN: %d\n",
				cpu, ret);

		return ret;
	}

	if (state != RISCV_SSPM_ENABLED)
		return 0;

	ret = riscv_sspm_set_local(RISCV_SSPM_PMLEN);
	if (ret) {
		pr_err("Sspm: CPU%u failed to enable PMLEN=7: %d\n", cpu, ret);
		cleanup_ret = sbi_fwft_set(SBI_FWFT_POINTER_MASKING_PMLEN, 0, 0);
		if (cleanup_ret)
			pr_crit("Sspm: CPU%u failed to clean up PMLEN: %d\n",
				cpu, cleanup_ret);
	}

	return ret;
}

#ifdef CONFIG_RISCV_ISA_SSPM
int riscv_sspm_enable(void)
{
	int ret, rollback_ret;

	mutex_lock(&riscv_sspm_lock);

	if (!READ_ONCE(riscv_sspm_smp_ready)) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	if (riscv_sspm_state == RISCV_SSPM_ENABLED) {
		ret = 0;
		goto out_unlock;
	}

	if (riscv_sspm_state == RISCV_SSPM_FAILED ||
	    riscv_sspm_state == RISCV_SSPM_BROKEN) {
		ret = riscv_sspm_error;
		goto out_unlock;
	}

	cpus_read_lock();
	WRITE_ONCE(riscv_sspm_state, RISCV_SSPM_ENABLING);

	ret = sbi_fwft_set_online_cpus(SBI_FWFT_POINTER_MASKING_PMLEN,
				       RISCV_SSPM_PMLEN, 0);
	if (!ret) {
		WRITE_ONCE(riscv_sspm_state, RISCV_SSPM_ENABLED);
		cpus_read_unlock();
		goto out_unlock;
	}

	rollback_ret = sbi_fwft_set_online_cpus(SBI_FWFT_POINTER_MASKING_PMLEN,
						0, 0);
	pr_warn("Sspm: failed to enable PMLEN=7: %d\n", ret);
	if (rollback_ret) {
		pr_crit("Sspm: failed to roll back PMLEN: %d\n", rollback_ret);
		riscv_sspm_error = rollback_ret;
		WRITE_ONCE(riscv_sspm_state, RISCV_SSPM_BROKEN);
		ret = rollback_ret;
	} else {
		riscv_sspm_error = ret;
		WRITE_ONCE(riscv_sspm_state, RISCV_SSPM_FAILED);
	}

	cpus_read_unlock();

out_unlock:
	mutex_unlock(&riscv_sspm_lock);

	return ret;
}

bool riscv_sspm_enabled(void)
{
	return READ_ONCE(riscv_sspm_state) == RISCV_SSPM_ENABLED;
}
#endif
