// SPDX-License-Identifier: GPL-2.0
/*
 * Software PPM for UCSI teardown-ordering races
 *
 * Copyright (C) 2026 Iván Ezequiel Rodriguez <ivanrwcm25@gmail.com>
 *
 * Drives the real UCSI core (ucsi_create / ucsi_register / ucsi_unregister /
 * ucsi_destroy / ucsi_notify_common) through a software PPM so that the
 * teardown orderings discussed on the ucsi_acpi UAF thread can be compared
 * under KASAN, lockdep and KCSAN without a PNP0CA0 ACPI device.
 *
 * The backend plays the same role as ucsi_acpi.c: it owns the notify source
 * that signals ucsi->complete, and it is the thing that must be quiesced
 * correctly during teardown.
 *
 * Module parameter ucsi_race_test.scenario:
 *   0  quiesce connector changes, keep completions, then unlink (the fix)
 *   1  unlink the notify source before ucsi_unregister() (v1; stalls)
 *   2  no quiesce; late connector notify after ucsi_unregister() (UAF)
 *   3  baseline: nothing in flight, correct order
 *   4  concurrent notify storm during the fixed teardown
 *   5  mid-init teardown with the fix
 *   6  mid-init teardown with the v1 ordering (stalls)
 *   8  CCI mask: completion kept, connector change dropped + positive control
 *   9  notify_lock is a real barrier, not just a flag
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/kthread.h>

#include "ucsi.h"

#define RACE_NUM_CONNECTORS	2
#define RACE_TAG		"UCSI-RACE"

/* Connector Change Indication field of the CCI. */
#define UCSI_RACE_CONNECTOR_FIELD	GENMASK(7, 1)

static int scenario;
module_param(scenario, int, 0444);
MODULE_PARM_DESC(scenario,
		 "0=fixed 1=v1-unlink-first 2=late-notify-uaf 3=baseline 4=storm 5=mid-init 6=mid-init-v1 8=cci-mask 9=lock-barrier");

static int cycles = 5;
module_param(cycles, int, 0444);
MODULE_PARM_DESC(cycles, "teardown cycles for the storm scenario");

static int storm_delay_ms = 300;
module_param(storm_delay_ms, int, 0444);
MODULE_PARM_DESC(storm_delay_ms, "in-flight command delay per storm cycle");

struct race_ppm {
	struct device *dev;
	struct ucsi *ucsi;

	/* Mirrors ucsi_acpi: serialises notify against start of teardown. */
	struct mutex notify_lock;
	bool quiescing;
	bool unlinked;

	/*
	 * Scenario 9 only: hold notify_lock for this long inside the handler,
	 * so that the start of teardown provably has to wait for a notify that
	 * entered the barrier first.
	 */
	unsigned int block_ms;
	bool in_notify;

	/* Protects cci, last_cmd and oneshot_delay_ms. */
	spinlock_t cci_lock;
	u32 cci;
	u64 last_cmd;

	struct ucsi_capability cap;
	atomic_t cmds;

	struct workqueue_struct *notify_wq;
	struct delayed_work notify_work;
	/*
	 * Delay applied to the next command only, so that exactly one command
	 * is left in flight when teardown starts. Every other command is
	 * answered synchronously, which keeps the measured baseline low and
	 * makes a missing completion show up as the full UCSI_TIMEOUT_MS.
	 */
	unsigned int oneshot_delay_ms;
};

/*
 * CCI is a single register in which the connector-change field and the
 * command status bits coexist. Raising a connector change must therefore
 * not clobber a command completion that the OS has not acknowledged yet,
 * otherwise the emulator invents timeouts that no real PPM would produce.
 */
static void race_raise_connector_change(struct race_ppm *rp, u8 num)
{
	unsigned long flags;

	spin_lock_irqsave(&rp->cci_lock, flags);
	rp->cci = (rp->cci & ~UCSI_RACE_CONNECTOR_FIELD) |
		  (((u32)num << 1) & UCSI_RACE_CONNECTOR_FIELD);
	spin_unlock_irqrestore(&rp->cci_lock, flags);
}

static u32 race_get_cci(struct race_ppm *rp)
{
	unsigned long flags;
	u32 cci;

	spin_lock_irqsave(&rp->cci_lock, flags);
	cci = rp->cci;
	spin_unlock_irqrestore(&rp->cci_lock, flags);

	return cci;
}

/*
 * The backend notify path. This is the code under test: it is the exact
 * shape of ucsi_acpi_notify() after the fix.
 */
static void race_deliver_notify(struct race_ppm *rp)
{
	u32 cci;

	mutex_lock(&rp->notify_lock);

	/* Emulates the ACPI handler having been unlinked already. */
	if (rp->unlinked)
		goto out_unlock;

	if (rp->block_ms) {
		unsigned int block = rp->block_ms;

		rp->block_ms = 0;
		WRITE_ONCE(rp->in_notify, true);
		msleep(block);
	}

	cci = race_get_cci(rp);

	if (rp->quiescing)
		cci &= UCSI_CCI_BUSY | UCSI_CCI_ACK_COMPLETE |
		       UCSI_CCI_COMMAND_COMPLETE;

	ucsi_notify_common(rp->ucsi, cci);

out_unlock:
	mutex_unlock(&rp->notify_lock);
}

static void race_notify_work(struct work_struct *work)
{
	struct race_ppm *rp = container_of(to_delayed_work(work),
					   struct race_ppm, notify_work);

	race_deliver_notify(rp);
}

/* ------------------------- emulated PPM operations ----------------------- */

static int race_read_version(struct ucsi *ucsi, u16 *version)
{
	*version = UCSI_VERSION_1_2;
	return 0;
}

static int race_read_cci(struct ucsi *ucsi, u32 *cci)
{
	*cci = race_get_cci(ucsi_get_drvdata(ucsi));
	return 0;
}

static int race_poll_cci(struct ucsi *ucsi, u32 *cci)
{
	return race_read_cci(ucsi, cci);
}

static int race_read_message_in(struct ucsi *ucsi, void *val, size_t val_len)
{
	struct race_ppm *rp = ucsi_get_drvdata(ucsi);
	unsigned long flags;
	u64 cmd;

	spin_lock_irqsave(&rp->cci_lock, flags);
	cmd = rp->last_cmd;
	spin_unlock_irqrestore(&rp->cci_lock, flags);

	memset(val, 0, val_len);

	if (UCSI_COMMAND(cmd) == UCSI_GET_CAPABILITY)
		memcpy(val, &rp->cap, min(val_len, sizeof(rp->cap)));

	return 0;
}

static int race_write_message_out(struct ucsi *ucsi, void *data, size_t len)
{
	return 0;
}

/*
 * Accept any command and answer it with the matching completion. The
 * completion is delivered from notify_wq, which models the ACPI notify
 * workqueue: it is a context the teardown path has to cooperate with.
 */
static int race_async_control(struct ucsi *ucsi, u64 command)
{
	struct race_ppm *rp = ucsi_get_drvdata(ucsi);
	unsigned long flags;
	unsigned int delay;
	u32 cci;

	atomic_inc(&rp->cmds);

	spin_lock_irqsave(&rp->cci_lock, flags);

	/* A pending connector change survives until the OS acknowledges it. */
	cci = rp->cci & UCSI_RACE_CONNECTOR_FIELD;

	switch (UCSI_COMMAND(command)) {
	case UCSI_PPM_RESET:
		cci = UCSI_CCI_RESET_COMPLETE;
		break;
	case UCSI_ACK_CC_CI:
		if (command & UCSI_ACK_CONNECTOR_CHANGE)
			cci &= ~UCSI_RACE_CONNECTOR_FIELD;
		cci |= UCSI_CCI_ACK_COMPLETE;
		break;
	default:
		cci |= UCSI_CCI_COMMAND_COMPLETE | UCSI_SET_CCI_LENGTH(0x10);
		break;
	}

	rp->last_cmd = command;
	rp->cci = cci;

	/* PPM_RESET is polled, not notified, so it must not eat the delay. */
	if (UCSI_COMMAND(command) == UCSI_PPM_RESET) {
		delay = 0;
	} else {
		delay = rp->oneshot_delay_ms;
		rp->oneshot_delay_ms = 0;
	}

	spin_unlock_irqrestore(&rp->cci_lock, flags);

	/* PPM_RESET is observed by polling, no notify needed. */
	if (UCSI_COMMAND(command) == UCSI_PPM_RESET)
		return 0;

	if (delay)
		queue_delayed_work(rp->notify_wq, &rp->notify_work,
				   msecs_to_jiffies(delay));
	else
		race_deliver_notify(rp);

	return 0;
}

static const struct ucsi_operations race_ops = {
	.read_version = race_read_version,
	.read_cci = race_read_cci,
	.poll_cci = race_poll_cci,
	.read_message_in = race_read_message_in,
	.write_message_out = race_write_message_out,
	.sync_control = ucsi_sync_control_common,
	.async_control = race_async_control,
};

/* ------------------------------ the scenarios ---------------------------- */

/* Fire a connector-change notification for connector @num. */
static void race_fire_connector_change(struct race_ppm *rp, u8 num)
{
	race_raise_connector_change(rp, num);
	race_deliver_notify(rp);
}

static void race_unlink_notify(struct race_ppm *rp)
{
	/*
	 * Models acpi_remove_notify_handler(): stop dispatching and then wait
	 * for anything already dispatched. The flag must not be set while
	 * holding nothing else, and the flush must happen outside the lock,
	 * exactly like the real path.
	 */
	mutex_lock(&rp->notify_lock);
	rp->unlinked = true;
	mutex_unlock(&rp->notify_lock);

	cancel_delayed_work_sync(&rp->notify_work);
	flush_workqueue(rp->notify_wq);
}

static void race_teardown(struct race_ppm *rp)
{
	unsigned long flags;
	ktime_t t0;
	s64 ms;

	/*
	 * Put a connector work in flight: it issues GET_CONNECTOR_STATUS and
	 * blocks in wait_for_completion_timeout() until notify_wq answers.
	 * Scenario 3 skips this to measure the teardown floor.
	 */
	if (scenario != 3) {
		spin_lock_irqsave(&rp->cci_lock, flags);
		rp->oneshot_delay_ms = 1500;
		spin_unlock_irqrestore(&rp->cci_lock, flags);

		race_fire_connector_change(rp, 1);

		/* Let the work reach the wait before teardown starts. */
		msleep(80);

		/*
		 * The whole test is meaningless unless a connector work is
		 * really parked in wait_for_completion_timeout() right now.
		 */
		pr_info(RACE_TAG ": ntfy=0x%llx flags=0x%lx conn_change_en=%d\n",
			rp->ucsi->ntfy, rp->ucsi->flags,
			!!(rp->ucsi->ntfy & UCSI_ENABLE_NTFY_CONNECTOR_CHANGE));
		pr_info(RACE_TAG ": cmds=%d notify_pending=%d cmd_pending=%d event_pending=%d\n",
			atomic_read(&rp->cmds),
			!!delayed_work_pending(&rp->notify_work),
			test_bit(COMMAND_PENDING, &rp->ucsi->flags),
			test_bit(EVENT_PENDING, &rp->ucsi->flags));

		if (delayed_work_pending(&rp->notify_work) &&
		    test_bit(COMMAND_PENDING, &rp->ucsi->flags))
			pr_info(RACE_TAG ": PRECOND=OK work parked on completion\n");
		else
			pr_err(RACE_TAG ": PRECOND=FAIL no work parked on completion\n");
	}

	pr_info(RACE_TAG ": scenario %d teardown start\n", scenario);
	t0 = ktime_get();

	switch (scenario) {
	case 3:
		/* Baseline: nothing in flight, correct order. */
		mutex_lock(&rp->notify_lock);
		rp->quiescing = true;
		mutex_unlock(&rp->notify_lock);

		ucsi_unregister(rp->ucsi);
		race_unlink_notify(rp);
		break;
	case 1:
		/*
		 * The v1 ordering: kill the notify source first. The connector
		 * work is now waiting for a completion that can never arrive,
		 * so ucsi_unregister() must sit through UCSI_TIMEOUT_MS.
		 */
		race_unlink_notify(rp);
		ucsi_unregister(rp->ucsi);
		break;
	case 2:
		/*
		 * Handler kept alive across unregister but connector changes
		 * are never suppressed. The late notify below reaches
		 * ucsi_connector_change() after the connector array was freed.
		 */
		ucsi_unregister(rp->ucsi);
		pr_info(RACE_TAG ": firing late connector notify after unregister\n");
		race_fire_connector_change(rp, 1);
		race_unlink_notify(rp);
		break;
	default:
		/*
		 * The fix: suppress connector changes, keep completions, drain,
		 * then unlink and flush before the object is freed.
		 */
		mutex_lock(&rp->notify_lock);
		rp->quiescing = true;
		mutex_unlock(&rp->notify_lock);

		ucsi_unregister(rp->ucsi);

		/* A late notify must now be harmless even with connector bits. */
		pr_info(RACE_TAG ": firing late connector notify after unregister\n");
		race_fire_connector_change(rp, 1);

		race_unlink_notify(rp);
		break;
	}

	ms = ktime_ms_delta(ktime_get(), t0);
	pr_info(RACE_TAG ": teardown_ms=%lld\n", ms);

	if (ms >= 5000)
		pr_err(RACE_TAG ": RESULT=STALL teardown blocked %lld ms\n", ms);
	else
		pr_info(RACE_TAG ": RESULT=NOSTALL teardown %lld ms\n", ms);

	ucsi_destroy(rp->ucsi);
	rp->ucsi = NULL;

	pr_info(RACE_TAG ": destroy done\n");
}

/*
 * Scenario 4: hammer the notify path from several CPUs while the fixed
 * teardown runs, repeatedly. Any missing barrier in the quiesce protocol
 * shows up as a KASAN report on the connector array or on the ucsi object,
 * or as a lockdep splat on notify_lock.
 */
#define RACE_STORM_THREADS	4

static int race_storm_thread(void *data)
{
	struct race_ppm *rp = data;

	while (!kthread_should_stop()) {
		race_raise_connector_change(rp, 1);
		race_deliver_notify(rp);
		usleep_range(20, 120);
	}

	return 0;
}

static void race_storm_cycle(struct race_ppm *rp, int cycle)
{
	struct task_struct *th[RACE_STORM_THREADS];
	unsigned long flags;
	int i, n = 0;

	rp->quiescing = false;
	rp->unlinked = false;

	rp->ucsi = ucsi_create(rp->dev, &race_ops);
	if (IS_ERR(rp->ucsi)) {
		pr_err(RACE_TAG ": cycle %d ucsi_create failed\n", cycle);
		rp->ucsi = NULL;
		return;
	}
	ucsi_set_drvdata(rp->ucsi, rp);

	if (ucsi_register(rp->ucsi)) {
		pr_err(RACE_TAG ": cycle %d ucsi_register failed\n", cycle);
		goto out_destroy;
	}

	for (i = 0; i < 500; i++) {
		if (rp->ucsi->connector && rp->ucsi->ntfy)
			break;
		msleep(20);
	}

	if (!rp->ucsi->connector || !rp->ucsi->ntfy) {
		pr_err(RACE_TAG ": cycle %d init did not land\n", cycle);
		ucsi_unregister(rp->ucsi);
		race_unlink_notify(rp);
		goto out_destroy;
	}

	for (i = 0; i < RACE_STORM_THREADS; i++) {
		th[n] = kthread_run(race_storm_thread, rp, "ucsi_storm%d", i);
		if (!IS_ERR(th[n]))
			n++;
	}

	/* Park one command mid-flight, as in the single-shot scenarios. */
	spin_lock_irqsave(&rp->cci_lock, flags);
	rp->oneshot_delay_ms = storm_delay_ms;
	spin_unlock_irqrestore(&rp->cci_lock, flags);

	msleep(min(storm_delay_ms / 4 + 1, 60));

	/* The fix, under concurrent notifies. */
	mutex_lock(&rp->notify_lock);
	rp->quiescing = true;
	mutex_unlock(&rp->notify_lock);

	ucsi_unregister(rp->ucsi);

	race_unlink_notify(rp);

	for (i = 0; i < n; i++)
		kthread_stop(th[i]);

out_destroy:
	ucsi_destroy(rp->ucsi);
	rp->ucsi = NULL;
	if (cycles <= 20 || !((cycle + 1) % 100))
		pr_info(RACE_TAG ": cycle %d done\n", cycle);
}

/*
 * Scenario 8: deterministic proof of what the CCI mask does and does not do.
 *
 * A single CCI carrying both a connector change and a command completion is
 * delivered while quiescing. The completion must still be signalled and the
 * connector path must not be entered. The same CCI is then delivered with
 * quiescing off as a positive control, so that a test that silently stopped
 * exercising ucsi_connector_change() cannot pass.
 */
static void race_prove_mask(struct race_ppm *rp)
{
	struct ucsi *ucsi = rp->ucsi;
	bool ev_quiesced, cmd_cleared, ev_control;
	/* UCSI_CCI_CONNECTOR() extracts the field, so build it by hand. */
	u32 cci = ((u32)1 << 1) | UCSI_CCI_COMMAND_COMPLETE;
	unsigned long flags;

	/* Deliver the crafted CCI rather than whatever init left behind. */
	spin_lock_irqsave(&rp->cci_lock, flags);
	rp->cci = cci;
	spin_unlock_irqrestore(&rp->cci_lock, flags);

	pr_info(RACE_TAG ": mask test cci=0x%08x connector=%u\n",
		cci, (unsigned int)UCSI_CCI_CONNECTOR(cci));

	/* Quiescing: completion expected, connector dispatch not. */
	clear_bit(EVENT_PENDING, &ucsi->flags);
	set_bit(COMMAND_PENDING, &ucsi->flags);
	reinit_completion(&ucsi->complete);

	mutex_lock(&rp->notify_lock);
	rp->quiescing = true;
	mutex_unlock(&rp->notify_lock);

	race_deliver_notify(rp);

	ev_quiesced = test_bit(EVENT_PENDING, &ucsi->flags);
	cmd_cleared = !test_bit(COMMAND_PENDING, &ucsi->flags);

	pr_info(RACE_TAG ": quiesced: event_pending=%d command_completed=%d\n",
		ev_quiesced, cmd_cleared);

	/* Positive control: same CCI, quiescing off, connector must dispatch. */
	clear_bit(EVENT_PENDING, &ucsi->flags);
	set_bit(COMMAND_PENDING, &ucsi->flags);
	reinit_completion(&ucsi->complete);

	mutex_lock(&rp->notify_lock);
	rp->quiescing = false;
	mutex_unlock(&rp->notify_lock);

	race_deliver_notify(rp);

	ev_control = test_bit(EVENT_PENDING, &ucsi->flags);

	pr_info(RACE_TAG ": control: event_pending=%d\n", ev_control);

	if (!ev_quiesced && cmd_cleared && ev_control)
		pr_info(RACE_TAG ": RESULT=MASKOK completion kept, connector dropped\n");
	else
		pr_err(RACE_TAG ": RESULT=MASKFAIL quiesced_ev=%d cmd=%d control_ev=%d\n",
		       ev_quiesced, cmd_cleared, ev_control);

	/* Let the control's connector work settle before tearing down. */
	msleep(200);

	mutex_lock(&rp->notify_lock);
	rp->quiescing = true;
	mutex_unlock(&rp->notify_lock);

	ucsi_unregister(rp->ucsi);
	race_unlink_notify(rp);
	ucsi_destroy(rp->ucsi);
	rp->ucsi = NULL;
}

static int race_block_thread(void *data)
{
	race_deliver_notify(data);
	return 0;
}

/*
 * Scenario 9: prove that notify_lock is a real barrier and not just a flag.
 *
 * A notify enters the handler before teardown and stays inside it. Setting
 * quiescing must not be able to complete until that notify has returned,
 * which is what makes "its schedule_work() happens before ucsi_unregister()
 * starts cancelling" true rather than merely likely.
 */
#define RACE_BLOCK_MS	1200

static void race_prove_barrier(struct race_ppm *rp)
{
	struct task_struct *th;
	ktime_t t0;
	s64 waited;
	int i;

	race_raise_connector_change(rp, 1);

	mutex_lock(&rp->notify_lock);
	rp->block_ms = RACE_BLOCK_MS;
	rp->in_notify = false;
	mutex_unlock(&rp->notify_lock);

	th = kthread_run(race_block_thread, rp, "ucsi_block");
	if (IS_ERR(th)) {
		pr_err(RACE_TAG ": RESULT=BARRIERFAIL cannot start notify\n");
		return;
	}

	/* Wait until the notify is provably inside the critical section. */
	for (i = 0; i < 500; i++) {
		if (READ_ONCE(rp->in_notify))
			break;
		usleep_range(1000, 2000);
	}

	if (!READ_ONCE(rp->in_notify)) {
		pr_err(RACE_TAG ": RESULT=BARRIERFAIL notify never entered\n");
		return;
	}

	pr_info(RACE_TAG ": notify inside barrier, starting teardown\n");

	t0 = ktime_get();
	mutex_lock(&rp->notify_lock);
	rp->quiescing = true;
	mutex_unlock(&rp->notify_lock);
	waited = ktime_ms_delta(ktime_get(), t0);

	pr_info(RACE_TAG ": quiesce waited %lld ms for the in-flight notify\n",
		waited);

	if (waited >= RACE_BLOCK_MS / 2)
		pr_info(RACE_TAG ": RESULT=BARRIEROK teardown blocked %lld ms\n",
			waited);
	else
		pr_err(RACE_TAG ": RESULT=BARRIERFAIL teardown raced past, %lld ms\n",
		       waited);

	ucsi_unregister(rp->ucsi);
	race_unlink_notify(rp);
	ucsi_destroy(rp->ucsi);
	rp->ucsi = NULL;
}

static struct platform_device *race_pdev;

static int __init race_init(void)
{
	struct race_ppm *rp;
	int ret;

	race_pdev = platform_device_register_simple("ucsi_race", -1, NULL, 0);
	if (IS_ERR(race_pdev))
		return PTR_ERR(race_pdev);

	rp = kzalloc_obj(*rp);
	if (!rp) {
		ret = -ENOMEM;
		goto err_pdev;
	}

	rp->dev = &race_pdev->dev;
	mutex_init(&rp->notify_lock);
	spin_lock_init(&rp->cci_lock);
	INIT_DELAYED_WORK(&rp->notify_work, race_notify_work);
	rp->cap.num_connectors = RACE_NUM_CONNECTORS;

	/* kacpi_notify_wq is WQ_PERCPU with default concurrency. */
	rp->notify_wq = alloc_workqueue("ucsi_race_notify", WQ_PERCPU, 0);
	if (!rp->notify_wq) {
		ret = -ENOMEM;
		goto err_free;
	}

	if (scenario == 4) {
		int c;

		for (c = 0; c < cycles; c++)
			race_storm_cycle(rp, c);

		pr_info(RACE_TAG ": RESULT=STORMDONE %d cycles %d threads\n",
			cycles, RACE_STORM_THREADS);
		goto out_clean;
	}

	rp->ucsi = ucsi_create(rp->dev, &race_ops);
	if (IS_ERR(rp->ucsi)) {
		ret = PTR_ERR(rp->ucsi);
		goto err_wq;
	}
	ucsi_set_drvdata(rp->ucsi, rp);

	ret = ucsi_register(rp->ucsi);
	if (ret) {
		pr_err(RACE_TAG ": RESULT=REGFAIL ucsi_register returned %d\n", ret);
		ucsi_destroy(rp->ucsi);
		goto err_wq;
	}

	/*
	 * Scenario 5: tear down while ucsi_init_work() is still running its
	 * command sequence. ucsi_unregister() starts with
	 * cancel_delayed_work_sync(&ucsi->work), so the init work must be able
	 * to finish its in-flight command, which again needs the notify path
	 * to stay alive across ucsi_unregister().
	 */
	if (scenario == 5 || scenario == 6) {
		unsigned long flags;
		ktime_t t0;
		s64 ms;

		/* Park one of the init commands in wait_for_completion(). */
		spin_lock_irqsave(&rp->cci_lock, flags);
		rp->oneshot_delay_ms = 1500;
		spin_unlock_irqrestore(&rp->cci_lock, flags);

		msleep(40);

		pr_info(RACE_TAG ": mid-init connector=%p ntfy=0x%llx cmds=%d notify_pending=%d cmd_pending=%d\n",
			rp->ucsi->connector, rp->ucsi->ntfy,
			atomic_read(&rp->cmds),
			!!delayed_work_pending(&rp->notify_work),
			test_bit(COMMAND_PENDING, &rp->ucsi->flags));

		if (delayed_work_pending(&rp->notify_work) &&
		    test_bit(COMMAND_PENDING, &rp->ucsi->flags))
			pr_info(RACE_TAG ": PRECOND=OK init command parked\n");
		else
			pr_err(RACE_TAG ": PRECOND=FAIL no init command parked\n");

		t0 = ktime_get();

		if (scenario == 6) {
			/* v1 ordering: the parked init command loses its notify. */
			race_unlink_notify(rp);
			ucsi_unregister(rp->ucsi);
		} else {
			mutex_lock(&rp->notify_lock);
			rp->quiescing = true;
			mutex_unlock(&rp->notify_lock);

			ucsi_unregister(rp->ucsi);
			race_unlink_notify(rp);
		}

		ms = ktime_ms_delta(ktime_get(), t0);
		pr_info(RACE_TAG ": teardown_ms=%lld\n", ms);
		if (ms >= 5000)
			pr_err(RACE_TAG ": RESULT=STALL mid-init %lld ms\n", ms);
		else
			pr_info(RACE_TAG ": RESULT=NOSTALL mid-init %lld ms\n", ms);

		ucsi_destroy(rp->ucsi);
		rp->ucsi = NULL;
		goto out_clean;
	}

	/*
	 * ucsi_register() only queues ucsi_init_work(); the connectors and the
	 * notification mask appear later. Wait for init to land, otherwise
	 * ucsi_connector_change() would bail out on the ntfy gate and no work
	 * would ever be queued.
	 */
	for (ret = 0; ret < 500; ret++) {
		if (rp->ucsi->connector && rp->ucsi->ntfy)
			break;
		msleep(20);
	}

	if (!rp->ucsi->connector || !rp->ucsi->ntfy) {
		pr_err(RACE_TAG ": RESULT=INITFAIL ntfy=0x%llx connector=%p\n",
		       rp->ucsi->ntfy, rp->ucsi->connector);
		ucsi_unregister(rp->ucsi);
		race_unlink_notify(rp);
		ucsi_destroy(rp->ucsi);
		ret = -ENODEV;
		goto err_wq;
	}

	pr_info(RACE_TAG ": init complete, %u connectors, ntfy=0x%llx\n",
		rp->ucsi->cap.num_connectors, rp->ucsi->ntfy);

	if (scenario == 8)
		race_prove_mask(rp);
	else if (scenario == 9)
		race_prove_barrier(rp);
	else
		race_teardown(rp);

out_clean:
	destroy_workqueue(rp->notify_wq);
	kfree(rp);
	platform_device_unregister(race_pdev);

	pr_info(RACE_TAG ": DONE\n");
	return 0;

err_wq:
	destroy_workqueue(rp->notify_wq);
err_free:
	kfree(rp);
err_pdev:
	platform_device_unregister(race_pdev);
	return ret;
}

static void __exit race_exit(void)
{
}

module_init(race_init);
module_exit(race_exit);

MODULE_DESCRIPTION("UCSI teardown race harness");
MODULE_LICENSE("GPL v2");
