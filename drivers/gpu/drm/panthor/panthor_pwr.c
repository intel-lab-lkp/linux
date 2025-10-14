// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2025 ARM Limited. All rights reserved. */

#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/wait.h>

#include <drm/drm_managed.h>

#include "panthor_device.h"
#include "panthor_hw.h"
#include "panthor_pwr.h"
#include "panthor_regs.h"

#define PWR_INTERRUPTS_MASK \
	(PWR_IRQ_POWER_CHANGED_SINGLE | \
	 PWR_IRQ_POWER_CHANGED_ALL | \
	 PWR_IRQ_DELEGATION_CHANGED | \
	 PWR_IRQ_RESET_COMPLETED | \
	 PWR_IRQ_RETRACT_COMPLETED | \
	 PWR_IRQ_INSPECT_COMPLETED | \
	 PWR_IRQ_COMMAND_NOT_ALLOWED | \
	 PWR_IRQ_COMMAND_INVALID)

#define PWR_ALL_CORES_MASK		GENMASK(63, 0)

#define PWR_DOMAIN_MAX_BITS		16

#define PWR_TRANSITION_TIMEOUT_US	2000000

#define PWR_RETRACT_TIMEOUT_US		2000

/**
 * struct panthor_pwr - PWR_CONTROL block management data.
 */
struct panthor_pwr {
	/** @irq: PWR irq. */
	struct panthor_irq irq;

	/** @reqs_lock: Lock protecting access to pending_reqs. */
	spinlock_t reqs_lock;

	/** @pending_reqs: Pending PWR requests. */
	u32 pending_reqs;

	/** @reqs_acked: PWR request wait queue. */
	wait_queue_head_t reqs_acked;
};

static void panthor_pwr_irq_handler(struct panthor_device *ptdev, u32 status)
{
	spin_lock(&ptdev->pwr->reqs_lock);
	gpu_write(ptdev, PWR_INT_CLEAR, status);

	if (unlikely(status & PWR_IRQ_COMMAND_NOT_ALLOWED))
		drm_err(&ptdev->base, "PWR_IRQ: COMMAND_NOT_ALLOWED");

	if (unlikely(status & PWR_IRQ_COMMAND_INVALID))
		drm_err(&ptdev->base, "PWR_IRQ: COMMAND_INVALID");

	if (status & ptdev->pwr->pending_reqs) {
		ptdev->pwr->pending_reqs &= ~status;
		wake_up_all(&ptdev->pwr->reqs_acked);
	}
	spin_unlock(&ptdev->pwr->reqs_lock);
}
PANTHOR_IRQ_HANDLER(pwr, PWR, panthor_pwr_irq_handler);

static u64 panthor_pwr_read_status(struct panthor_device *ptdev)
{
	return gpu_read64(ptdev, PWR_STATUS);
}

static void panthor_pwr_write_command(struct panthor_device *ptdev, u32 command, u64 args)
{
	if (args)
		gpu_write64(ptdev, PWR_CMDARG, args);

	gpu_write(ptdev, PWR_COMMAND, command);
}

static const char *get_domain_name(u8 domain)
{
	switch (domain) {
	case PWR_COMMAND_DOMAIN_L2:
		return "L2";
	case PWR_COMMAND_DOMAIN_TILER:
		return "Tiler";
	case PWR_COMMAND_DOMAIN_SHADER:
		return "Shader";
	case PWR_COMMAND_DOMAIN_BASE:
		return "Base";
	case PWR_COMMAND_DOMAIN_STACK:
		return "Stack";
	}
	return "Unknown";
}

static u32 get_domain_base(u8 domain)
{
	switch (domain) {
	case PWR_COMMAND_DOMAIN_L2:
		return PWR_L2_PRESENT;
	case PWR_COMMAND_DOMAIN_TILER:
		return PWR_TILER_PRESENT;
	case PWR_COMMAND_DOMAIN_SHADER:
		return PWR_SHADER_PRESENT;
	case PWR_COMMAND_DOMAIN_BASE:
		return PWR_BASE_PRESENT;
	case PWR_COMMAND_DOMAIN_STACK:
		return PWR_STACK_PRESENT;
	}
	return 0;
}

static u32 get_domain_ready_reg(u32 domain)
{
	return get_domain_base(domain) + (PWR_L2_READY - PWR_L2_PRESENT);
}

static u32 get_domain_pwrtrans_reg(u32 domain)
{
	return get_domain_base(domain) + (PWR_L2_PWRTRANS - PWR_L2_PRESENT);
}

static bool is_valid_domain(u32 domain)
{
	return get_domain_base(domain) != 0;
}

static bool has_rtu(struct panthor_device *ptdev)
{
	return ptdev->gpu_info.gpu_features & GPU_FEATURES_RAY_TRAVERSAL;
}

static u8 get_domain_subdomain(struct panthor_device *ptdev, u32 domain)
{
	if ((domain == PWR_COMMAND_DOMAIN_SHADER) && has_rtu(ptdev))
		return PWR_COMMAND_SUBDOMAIN_RTU;

	return 0;
}

static int panthor_pwr_domain_wait_transition(struct panthor_device *ptdev, u32 domain,
					      u32 timeout_us)
{
	u32 pwrtrans_reg = get_domain_pwrtrans_reg(domain);
	u64 val;
	int ret = 0;

	ret = gpu_read64_poll_timeout(ptdev, pwrtrans_reg, val,
				      !(PWR_ALL_CORES_MASK & val), 100,
				      timeout_us);
	if (ret) {
		drm_err(&ptdev->base, "%s domain power in transition, pwrtrans(0x%llx)",
			get_domain_name(domain), val);
		return ret;
	}

	return 0;
}

static void panthor_pwr_info_show(struct panthor_device *ptdev)
{
	drm_info(&ptdev->base, "GPU_FEATURES:    0x%016llx", ptdev->gpu_info.gpu_features);
	drm_info(&ptdev->base, "PWR_STATUS:      0x%016llx", gpu_read64(ptdev, PWR_STATUS));
	drm_info(&ptdev->base, "L2_PRESENT:      0x%016llx", gpu_read64(ptdev, PWR_L2_PRESENT));
	drm_info(&ptdev->base, "L2_PWRTRANS:     0x%016llx", gpu_read64(ptdev, PWR_L2_PWRTRANS));
	drm_info(&ptdev->base, "L2_READY:        0x%016llx", gpu_read64(ptdev, PWR_L2_READY));
	drm_info(&ptdev->base, "TILER_PRESENT:   0x%016llx", gpu_read64(ptdev, PWR_TILER_PRESENT));
	drm_info(&ptdev->base, "TILER_PWRTRANS:  0x%016llx", gpu_read64(ptdev, PWR_TILER_PWRTRANS));
	drm_info(&ptdev->base, "TILER_READY:     0x%016llx", gpu_read64(ptdev, PWR_TILER_READY));
	drm_info(&ptdev->base, "SHADER_PRESENT:  0x%016llx", gpu_read64(ptdev, PWR_SHADER_PRESENT));
	drm_info(&ptdev->base, "SHADER_PWRTRANS: 0x%016llx",
		 gpu_read64(ptdev, PWR_SHADER_PWRTRANS));
	drm_info(&ptdev->base, "SHADER_READY:    0x%016llx", gpu_read64(ptdev, PWR_SHADER_READY));
}

static int panthor_pwr_domain_transition(struct panthor_device *ptdev, u32 cmd, u32 domain,
					 u64 mask, u32 timeout_us)
{
	u32 ready_reg = get_domain_ready_reg(domain);
	u32 pwr_cmd = PWR_COMMAND_DEF(cmd, domain, get_domain_subdomain(ptdev, domain));
	u64 expected_val = cmd == PWR_COMMAND_POWER_DOWN ? 0 : mask;
	u64 val;
	int ret = 0;

	if (!is_valid_domain(domain))
		return -EINVAL;

	if (cmd != PWR_COMMAND_POWER_DOWN && cmd != PWR_COMMAND_POWER_UP) {
		drm_err(&ptdev->base, "Invalid power domain transition command (0x%x)", cmd);
		return -EINVAL;
	}

	ret = panthor_pwr_domain_wait_transition(ptdev, domain, timeout_us);
	if (ret)
		return ret;

	/* domain already in target state, return early */
	if ((gpu_read64(ptdev, ready_reg) & mask) == expected_val)
		return 0;

	panthor_pwr_write_command(ptdev, pwr_cmd, mask);

	ret = gpu_read64_poll_timeout(ptdev, ready_reg, val, (mask & val) == expected_val, 100,
				      timeout_us);
	if (ret) {
		drm_err(&ptdev->base, "timeout waiting on %s power %s, cmd(0x%x), arg(0x%llx)",
			get_domain_name(domain), cmd == PWR_COMMAND_POWER_DOWN ? "down" : "up",
			pwr_cmd, mask);
		panthor_pwr_info_show(ptdev);
		return ret;
	}

	return 0;
}

#define panthor_pwr_domain_power_off(__ptdev, __domain, __mask, __timeout_us)            \
	panthor_pwr_domain_transition(__ptdev, PWR_COMMAND_POWER_DOWN, __domain, __mask, \
				      __timeout_us)

#define panthor_pwr_domain_power_on(__ptdev, __domain, __mask, __timeout_us) \
	panthor_pwr_domain_transition(__ptdev, PWR_COMMAND_POWER_UP, __domain, __mask, __timeout_us)

/**
 * retract_domain() - Retract control of a domain from MCU
 * @ptdev: Device.
 * @domain: Domain to retract the control
 *
 * Retracting L2 domain is not expected since it won't be delegated.
 *
 * Return: 0 on success or retracted already.
 *         -EPERM if domain is L2.
 *         A negative error code otherwise.
 */
static int retract_domain(struct panthor_device *ptdev, u32 domain)
{
	const u32 pwr_cmd = PWR_COMMAND_DEF(PWR_COMMAND_RETRACT, domain, 0);
	const u32 pwr_status_reg = panthor_pwr_read_status(ptdev);
	const u32 delegated_mask = PWR_STATUS_DOMAIN_DELEGATED(domain);
	const u32 allow_mask = PWR_STATUS_DOMAIN_ALLOWED(domain);
	u64 val;
	int ret;

	if (WARN_ON(domain == PWR_COMMAND_DOMAIN_L2))
		return -EINVAL;

	if (WARN_ON(pwr_status_reg & PWR_STATUS_DOMAIN_DELEGATED(PWR_COMMAND_DOMAIN_L2)))
		return -EPERM;

	if (!(pwr_status_reg & delegated_mask)) {
		drm_dbg(&ptdev->base, "%s domain already retracted",
			get_domain_name(domain));
		return 0;
	}

	ret = gpu_read64_poll_timeout(ptdev, PWR_STATUS, val,
				      !(PWR_STATUS_RETRACT_PENDING & val), 0,
				      PWR_RETRACT_TIMEOUT_US);
	if (ret) {
		drm_err(&ptdev->base, "%s domain retract pending",
			get_domain_name(domain));
		return ret;
	}

	panthor_pwr_write_command(ptdev, pwr_cmd, 0);

	/*
	 * On successful retraction
	 * allow-flag will be set with delegated-flag being cleared.
	 */
	ret = gpu_read64_poll_timeout(ptdev, PWR_STATUS, val,
				      ((delegated_mask | allow_mask) & val) == allow_mask,
				      10, PWR_TRANSITION_TIMEOUT_US);
	if (ret) {
		drm_err(&ptdev->base, "Retracting %s domain timeout, cmd(0x%x)",
			get_domain_name(domain), pwr_cmd);
		return ret;
	}

	return 0;
}

/**
 * delegate_domain() - Delegate control of a domain to MCU
 * @ptdev: Device.
 * @domain: Domain to delegate the control
 *
 * Delegating L2 domain is prohibited.
 *
 * Return:
 * *       0 on success or delegated already.
 * *       -EPERM if domain is L2.
 * *       A negative error code otherwise.
 */
static int delegate_domain(struct panthor_device *ptdev, u32 domain)
{
	const u32 pwr_cmd = PWR_COMMAND_DEF(PWR_COMMAND_DELEGATE, domain, 0);
	const u32 pwr_status_reg = panthor_pwr_read_status(ptdev);
	const u32 pwrtrans_reg = get_domain_pwrtrans_reg(domain);
	const u32 allow_mask = PWR_STATUS_DOMAIN_ALLOWED(domain);
	const u32 delegated_mask = PWR_STATUS_DOMAIN_DELEGATED(domain);
	u64 val;
	int ret;

	if (WARN_ON_ONCE(domain == PWR_COMMAND_DOMAIN_L2))
		return -EINVAL;

	if (pwr_status_reg & delegated_mask) {
		drm_dbg(&ptdev->base, "%s domain already delegated",
			get_domain_name(domain));
		return 0;
	}

	/* Check if the command is allowed before delegating. */
	if (unlikely(!(pwr_status_reg & allow_mask))) {
		drm_warn(&ptdev->base, "Delegating %s domain not allowed",
			 get_domain_name(domain));
		return -EPERM;
	}

	if (unlikely(gpu_read64(ptdev, pwrtrans_reg))) {
		drm_warn(&ptdev->base, "%s domain power in transition",
			 get_domain_name(domain));
		return -EPERM;
	}

	panthor_pwr_write_command(ptdev, pwr_cmd, 0);

	/*
	 * On successful delegation
	 * allow-flag will be cleared with delegated-flag being set.
	 */
	ret = gpu_read64_poll_timeout(ptdev, PWR_STATUS, val,
				      ((delegated_mask | allow_mask) & val) == delegated_mask,
				      10, PWR_TRANSITION_TIMEOUT_US);
	if (ret) {
		drm_err(&ptdev->base, "Delegating %s domain timeout, cmd(0x%x)",
			get_domain_name(domain), pwr_cmd);
		return ret;
	}

	return 0;
}

static int panthor_pwr_delegate_domains(struct panthor_device *ptdev)
{
	int ret;

	if (!ptdev->pwr)
		return 0;

	ret = delegate_domain(ptdev, PWR_COMMAND_DOMAIN_SHADER);
	if (ret)
		return ret;

	ret = delegate_domain(ptdev, PWR_COMMAND_DOMAIN_TILER);
	if (ret)
		return ret;

	return 0;
}

void panthor_pwr_unplug(struct panthor_device *ptdev)
{
	unsigned long flags;

	if (!ptdev->pwr)
		return;

	/* Make sure the IRQ handler is not running after that point. */
	panthor_pwr_irq_suspend(&ptdev->pwr->irq);

	/* Wake-up all waiters. */
	spin_lock_irqsave(&ptdev->pwr->reqs_lock, flags);
	ptdev->pwr->pending_reqs = 0;
	wake_up_all(&ptdev->pwr->reqs_acked);
	spin_unlock_irqrestore(&ptdev->pwr->reqs_lock, flags);
}

int panthor_pwr_init(struct panthor_device *ptdev)
{
	struct panthor_pwr *pwr;
	int err, irq;

	if (!panthor_hw_has_feature(ptdev, PANTHOR_HW_FEATURE_PWR_CONTROL))
		return 0;

	pwr = drmm_kzalloc(&ptdev->base, sizeof(*pwr), GFP_KERNEL);
	if (!pwr)
		return -ENOMEM;

	spin_lock_init(&pwr->reqs_lock);
	init_waitqueue_head(&pwr->reqs_acked);
	ptdev->pwr = pwr;

	irq = platform_get_irq_byname(to_platform_device(ptdev->base.dev), "gpu");
	if (irq < 0)
		return irq;

	err = panthor_request_pwr_irq(ptdev, &pwr->irq, irq, PWR_INTERRUPTS_MASK);
	if (err)
		return err;

	return 0;
}

int panthor_pwr_reset_soft(struct panthor_device *ptdev)
{
	return 0;
}

int panthor_pwr_l2_power_off(struct panthor_device *ptdev)
{
	const u32 l2_allow_mask = PWR_STATUS_DOMAIN_ALLOWED(PWR_COMMAND_DOMAIN_L2);
	const u32 l2_pwrtrans_reg = get_domain_pwrtrans_reg(PWR_COMMAND_DOMAIN_L2);
	unsigned long child_domain_mask = {
		BIT(PWR_COMMAND_DOMAIN_SHADER) |
		BIT(PWR_COMMAND_DOMAIN_TILER)
	};
	u32 pwr_status, child_domain;
	int ret = 0;

	if (unlikely(!ptdev->pwr))
		return -ENODEV;

	pwr_status = panthor_pwr_read_status(ptdev);

	/* Abort if L2 power off constraints are not satisfied */
	if (!(pwr_status & l2_allow_mask)) {
		drm_warn(&ptdev->base, "Power off L2 domain not allowed");
		return 0;
	}

	if (gpu_read64(ptdev, l2_pwrtrans_reg)) {
		drm_warn(&ptdev->base, "L2 Power in transition");
		return 0;
	}

	/* It is expected that when halting the MCU, it would power down its
	 * delegated domains. However, an unresponsive or hung MCU may not do
	 * so, which is why we need to check and retract the domains back into
	 * host control to be powered down before powering down the L2.
	 */
	for_each_set_bit(child_domain, &child_domain_mask, PWR_DOMAIN_MAX_BITS) {
		u64 domain_ready = gpu_read64(ptdev, get_domain_ready_reg(child_domain));

		if (domain_ready && (pwr_status & PWR_STATUS_DOMAIN_DELEGATED(child_domain))) {
			drm_warn(&ptdev->base,
				 "L2 power off: Delegated %s domain not powered down by MCU",
				 get_domain_name(child_domain));
			ret = retract_domain(ptdev, child_domain);
			if (ret) {
				drm_err(&ptdev->base, "Failed to retract %s domain",
					get_domain_name(child_domain));
				panthor_pwr_info_show(ptdev);
				return ret;
			}
		}

		ret = panthor_pwr_domain_power_off(ptdev, child_domain, domain_ready,
						   PWR_TRANSITION_TIMEOUT_US);
		if (ret)
			return ret;
	}

	return panthor_pwr_domain_power_off(ptdev, PWR_COMMAND_DOMAIN_L2,
					    ptdev->gpu_info.l2_present,
					    PWR_TRANSITION_TIMEOUT_US);
}

int panthor_pwr_l2_power_on(struct panthor_device *ptdev)
{
	const u32 pwr_status = panthor_pwr_read_status(ptdev);
	const u32 l2_allow_mask = PWR_STATUS_DOMAIN_ALLOWED(PWR_COMMAND_DOMAIN_L2);
	const u32 l2_pwrtrans_reg = get_domain_pwrtrans_reg(PWR_COMMAND_DOMAIN_L2);
	int ret;

	if (!ptdev->pwr)
		return 0;

	if ((pwr_status & l2_allow_mask) == 0) {
		drm_warn(&ptdev->base, "Power on L2 domain not allowed");
		return -EPERM;
	}

	if (gpu_read64(ptdev, l2_pwrtrans_reg)) {
		drm_warn(&ptdev->base, "L2 Power is in transition");
		return -EINVAL;
	}

	ret = panthor_pwr_domain_power_on(ptdev, PWR_COMMAND_DOMAIN_L2,
					  ptdev->gpu_info.l2_present,
					  PWR_TRANSITION_TIMEOUT_US);
	if (ret)
		return ret;

	/* Delegate control of the shader and tiler power domains to the MCU as
	 * it can better manage which shader/tiler cores need to be powered up
	 * or can be powered down based on currently running jobs.
	 */
	return panthor_pwr_delegate_domains(ptdev);
}

void panthor_pwr_suspend(struct panthor_device *ptdev)
{
	if (!ptdev->pwr)
		return;

	panthor_pwr_irq_suspend(&ptdev->pwr->irq);
}

void panthor_pwr_resume(struct panthor_device *ptdev)
{
	if (!ptdev->pwr)
		return;

	panthor_pwr_irq_resume(&ptdev->pwr->irq, PWR_INTERRUPTS_MASK);
}
