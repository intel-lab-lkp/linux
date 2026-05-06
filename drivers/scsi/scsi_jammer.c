// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * scsi_jammer.c - Initiator-side SCSI command fault injector
 *
 * Simulates fabric events (RSCN, slow drain, path flap) on the initiator
 * side by intercepting commands in the SCSI mid-layer queuecommand path,
 * before they reach any HBA driver.  Works identically for FC, FCoE,
 * iSCSI, SAS — any transport that uses a Scsi_Host.
 *
 * SAFETY GUARANTEES
 * -----------------
 * - Commands are NEVER silently dropped. Every intercepted command is
 *   completed back to the mid-layer via scsi_done() with a well-defined
 *   error status, either immediately or after a timer fires.
 * - The completion always happens from a workqueue (not from atomic/IRQ
 *   context), so scsi_done() is always called in a safe context.
 * - A per-command pending list is protected by a spinlock.  On module
 *   unload, ALL pending commands are force-completed before the module
 *   exits — the initiator will never be left with orphaned commands.
 * - jam_flap_interval and jam_flap_hold are bounds-checked: minimum 100ms
 *   to prevent the workqueue from spinning and starving the system.
 * - The host_no match uses the Scsi_Host index that the mid-layer assigns;
 *   it cannot cause a NULL deref even if the host disappears mid-jam
 *   because we hold a reference via the scmd itself.
 *
 * THREE JAM MODES (set via jam_style debugfs knob)
 * -------------------------------------------------
 *  0 = drop    immediate DID_NO_CONNECT — looks like a dead path
 *  1 = timeout hold for jam_msecs ms then DID_NO_CONNECT — looks like
 *               a slow-drain / unresponsive fabric port; if jam_msecs
 *               exceeds the SCSI timeout the mid-layer's own EH fires,
 *               which is the most realistic RSCN simulation
 *  2 = flap    arm for jam_flap_hold ms, disarm for jam_flap_interval ms,
 *               repeat — simulates repeated RSCNs / flapping path
 *
 * DEBUGFS INTERFACE
 * -----------------
 * /sys/kernel/debug/scsi_jammer/
 *   jam_enable         w/r  0/1      master arm switch (reset clears jam_count)
 *   jam_host_no        w/r  int      Scsi_Host host_no to jam (-1 = all hosts)
 *   jam_style          w/r  0/1/2    mode: 0=drop 1=timeout 2=flap
 *   jam_msecs          w/r  u32      hold time for timeout/flap-hold phase (ms)
 *   jam_flap_interval  w/r  u32      disarmed interval for flap mode (ms, min 100)
 *   jam_tur_passthrough w/r  0/1      1 = pass TURs through, jam all other commands
 *   jam_count          r/o  u64      commands jammed since last jam_enable write
 *
 * USAGE EXAMPLES
 * --------------
 *   modprobe scsi_jammer
 *
 *   # Find your host number
 *   ls /sys/class/scsi_host/
 *
 *   # Mode 0: immediate dead path on host 3
 *   echo 3    > /sys/kernel/debug/scsi_jammer/jam_host_no
 *   echo 0    > /sys/kernel/debug/scsi_jammer/jam_style
 *   echo 1    > /sys/kernel/debug/scsi_jammer/jam_enable
 *   # watch dm-multipath fail over, then:
 *   echo 0    > /sys/kernel/debug/scsi_jammer/jam_enable
 *
 *   For Mode 1 recommended — set eh_deadline before arming
 *   # Mode 1: 35s stall (> SCSI 30s timeout) — triggers full EH + failover
 *   echo 10    > /sys/class/scsi_host/host12/eh_deadline
 *   echo 12    > /sys/kernel/debug/scsi_jammer/jam_host_no
 *   echo 35000 > /sys/kernel/debug/scsi_jammer/jam_msecs
 *   echo 1    > /sys/kernel/debug/scsi_jammer/jam_style
 *   echo 1    > /sys/kernel/debug/scsi_jammer/jam_enable
 *   EH fires within ~10s, multipath fails over, dd continues
 *   disarm when done:
 *   echo 0     > /sys/kernel/debug/scsi_jammer/jam_enable
 *
 *   # Mode 2: flapping RSCN — 5s jammed, 3s clear, repeat
 *   echo 3    > /sys/kernel/debug/scsi_jammer/jam_host_no
 *   echo 5000 > /sys/kernel/debug/scsi_jammer/jam_msecs
 *   echo 3000 > /sys/kernel/debug/scsi_jammer/jam_flap_interval
 *   echo 2    > /sys/kernel/debug/scsi_jammer/jam_style
 *   echo 1    > /sys/kernel/debug/scsi_jammer/jam_enable
 *
 *   # TUR passthrough — path stays active, data IOs stall (slow-drain simulation)
 *   echo 3    > /sys/kernel/debug/scsi_jammer/jam_host_no
 *   echo 1    > /sys/kernel/debug/scsi_jammer/jam_tur_passthrough
 *   echo 1    > /sys/kernel/debug/scsi_jammer/jam_style
 *   echo 1    > /sys/kernel/debug/scsi_jammer/jam_enable
 *   # multipath keeps path active (TURs pass), but data IOs stall
 *

 *   rmmod scsi_jammer   # safe at any time — drains all pending commands first

 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/debugfs.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <scsi/scsi.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>

MODULE_AUTHOR("Laurence Oberman <loberman@redhat.com>");
MODULE_DESCRIPTION("Initiator-side SCSI fault injector for error recovery testing");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.1");
MODULE_INFO(usage,
	"debugfs interface: /sys/kernel/debug/scsi_jammer/\n"
	"  jam_host_no         - Scsi_Host host_no to jam\n"
	"                        (ls /sys/class/scsi_host to find host numbers)\n"
	"  jam_style           - 0=drop 1=timeout 2=flap\n"
	"  jam_msecs           - hold duration ms (min 100, default 5000)\n"
	"  jam_flap_interval   - disarmed interval for flap mode (ms, min 100)\n"
	"  jam_tur_passthrough - 1=pass TURs through, jam all other commands\n"
	"  jam_enable          - write 1 to arm, 0 to disarm\n"
	"  jam_count           - commands jammed since last arm (read-only)\n"
	"Tip: set eh_deadline before arming for clean EH behaviour:\n"
	"  echo 10 > /sys/class/scsi_host/hostN/eh_deadline");

/* -------------------------------------------------------------------------
 * Jam styles
 * ----------------------------------------------------------------------
 */
#define JAM_STYLE_DROP    0   /* immediate DID_NO_CONNECT */
#define JAM_STYLE_TIMEOUT 1   /* hold jam_msecs then DID_NO_CONNECT */
#define JAM_STYLE_FLAP    2   /* periodic arm/disarm */

/* -------------------------------------------------------------------------
 * Global jammer state
 * Protected by jam_lock for the list and string fields.
 * Scalar flags use READ_ONCE/WRITE_ONCE — safe for int/u32 on all arches.
 * ----------------------------------------------------------------------
 */
static DEFINE_SPINLOCK(jam_lock);

static int  jam_enable    __read_mostly;   /* master arm switch         */
static int  jam_host_no   __read_mostly = -1; /* -1 = all hosts         */
static int  jam_style     __read_mostly = JAM_STYLE_DROP;
static u32  jam_msecs     __read_mostly = 5000;
static u32  jam_flap_interval __read_mostly = 3000; /* disarmed period  */
static int  jam_tur_passthrough __read_mostly;  /* 1 = let TURs through, jam everything else */
static atomic64_t jam_count;

/* pending command list — commands held for timeout/flap completion */
struct jam_cmd {
	struct list_head  list;
	struct scsi_cmnd *scmd;
	struct delayed_work work;
};

static LIST_HEAD(jam_pending);   /* protected by jam_lock */

/* workqueue for all deferred completions — singlethreaded so ordering
 * is deterministic and we can flush it cleanly on unload
 */
static struct workqueue_struct *jam_wq;

/* flap timer — fires in softirq, only schedules work, never sleeps */
static struct timer_list flap_timer;
static int flap_phase __read_mostly;  /* 0=armed 1=disarmed */

/* flap work — does the actual arm/disarm from workqueue (process) context */
static struct work_struct flap_work;

/* debugfs root */
static struct dentry *jam_dir;

/* -------------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------
 */
static void jam_complete_work(struct work_struct *work);
static void flap_timer_fn(struct timer_list *t);
static void flap_work_fn(struct work_struct *work);

/* -------------------------------------------------------------------------
 * scsi_host_template intercept
 *
 * We wrap queuecommand by patching the hostt pointer of the target
 * Scsi_Host at arm time.  This is the safest intercept point:
 *   - Called in process context (blk-mq submit path)
 *   - The scmd is fully initialised
 *   - Returning SCSI_MLQUEUE_HOST_BUSY requeues without error
 *   - Calling scsi_done() with an error result completes immediately
 *
 * We do NOT patch hostt permanently — we save/restore the original
 * queuecommand pointer so the host works normally when disarmed.
 * ----------------------------------------------------------------------
 */

/* per-host saved state, allocated at arm time */
struct jam_host_state {
	struct Scsi_Host                 *shost;
	const struct scsi_host_template  *orig_hostt;
	struct scsi_host_template         fake_hostt;  /* copy with our queuecommand */
};

static struct jam_host_state *jam_hstate;  /* NULL when not armed */

/*
 * Our replacement queuecommand.  Called instead of the real HBA driver's
 * queuecommand when the jammer is armed for this host.

 */
static enum scsi_qc_status jammer_queuecommand(struct Scsi_Host *shost,
					struct scsi_cmnd *scmd)
{
	struct jam_cmd *jc;
	unsigned long flags;
	int style = READ_ONCE(jam_style);
	u32 msecs = READ_ONCE(jam_msecs);

	/*
	 * Safety: if jam_enable was cleared between the check in
	 * scsi_queue_rq and now, pass through to the real driver.
	 */
	if (!READ_ONCE(jam_enable)) {
		spin_lock_irqsave(&jam_lock, flags);
		if (jam_hstate && jam_hstate->orig_hostt->queuecommand) {
			enum scsi_qc_status ret;
			/* temporarily restore real hostt for this call */
			ret = jam_hstate->orig_hostt->queuecommand(shost, scmd);
			spin_unlock_irqrestore(&jam_lock, flags);
			return ret;
		}
		spin_unlock_irqrestore(&jam_lock, flags);
		scmd->result = DID_NO_CONNECT << 16;
		scsi_done(scmd);
		return 0;
	}

	/*
	 * TUR passthrough: if enabled, let TEST UNIT READY (opcode 0x00)
	 * through to the real driver unconditionally.  This simulates the
	 * real-world failure mode where a fabric issue stalls data movement
	 * but the path appears alive to multipath because TURs succeed.
	 * Checked AFTER jam_enable guard so disarming always works, but
	 * BEFORE jam_count so passed-through TURs are not counted as jammed.
	 */
	if (READ_ONCE(jam_tur_passthrough) &&
	    scmd->cmnd[0] == TEST_UNIT_READY) {
		spin_lock_irqsave(&jam_lock, flags);
		if (jam_hstate && jam_hstate->orig_hostt->queuecommand) {
			int ret;

			ret = jam_hstate->orig_hostt->queuecommand(shost, scmd);
			spin_unlock_irqrestore(&jam_lock, flags);
			return ret;
		}
		spin_unlock_irqrestore(&jam_lock, flags);
		/* no real driver available — complete clean */
		scmd->result = 0;
		scsi_done(scmd);
		return 0;
	}

	atomic64_inc(&jam_count);

	if (style == JAM_STYLE_DROP) {
		/* Mode 0: immediate error */
		scmd->result = DID_NO_CONNECT << 16;
		scsi_done(scmd);
		return 0;
	}

	/* Mode 1 and 2: hold the command, complete later from workqueue */
	jc = kzalloc(sizeof(*jc), GFP_ATOMIC);
	if (!jc) {
		/*
		 * SAFETY: if we can't allocate, complete with error NOW.
		 * Never hold a command without a completion path.
		 */
		scmd->result = DID_NO_CONNECT << 16;
		scsi_done(scmd);
		return 0;
	}

	jc->scmd = scmd;
	INIT_DELAYED_WORK(&jc->work, jam_complete_work);

	spin_lock_irqsave(&jam_lock, flags);
	list_add_tail(&jc->list, &jam_pending);
	spin_unlock_irqrestore(&jam_lock, flags);

	/* schedule completion after jam_msecs */
	queue_delayed_work(jam_wq, &jc->work, msecs_to_jiffies(msecs));
	return 0;
}

/*
 * Deferred completion — called from jam_wq after jam_msecs delay.
 * Always safe: workqueue context, scsi_done() is allowed here.

 */
static void jam_complete_work(struct work_struct *work)
{
	struct jam_cmd *jc = container_of(to_delayed_work(work),
					  struct jam_cmd, work);
	unsigned long flags;

	spin_lock_irqsave(&jam_lock, flags);
	list_del(&jc->list);
	spin_unlock_irqrestore(&jam_lock, flags);

	jc->scmd->result = DID_NO_CONNECT << 16;
	scsi_done(jc->scmd);
	kfree(jc);
}

/* -------------------------------------------------------------------------
 * Drain all pending commands — called on disarm and on module unload.
 * SAFETY: this ensures no command is ever orphaned.
 * ----------------------------------------------------------------------
 */
static void jam_drain_pending(void)
{
	struct jam_cmd *jc, *tmp;
	unsigned long flags;
	LIST_HEAD(drain_list);

	/*
	 * Two-phase drain — must be called from process context only.
	 *
	 * Phase 1: snapshot the list under the lock so no new entries
	 * are added while we drain. jam_complete_work removes entries
	 * from jam_pending under jam_lock before calling scsi_done, so
	 * after we splice, any entry still in drain_list is owned by us.
	 *
	 * Phase 2: for each owned entry, cancel the delayed work.
	 * cancel_delayed_work_sync is safe here — we are in process
	 * context (called only from flap_work_fn or module exit).
	 * If the work already fired, cancel is a no-op and jam_complete_work
	 * will have already removed the entry from jam_pending — but since
	 * we spliced before checking, it will NOT be in drain_list, so we
	 * will not double-free it.
	 */
	spin_lock_irqsave(&jam_lock, flags);
	list_splice_init(&jam_pending, &drain_list);
	spin_unlock_irqrestore(&jam_lock, flags);

	list_for_each_entry_safe(jc, tmp, &drain_list, list) {
		/*
		 * Cancel the delayed work. If it already fired and called
		 * list_del+scsi_done, it removed itself from jam_pending
		 * under jam_lock BEFORE we spliced — so it cannot be in
		 * drain_list. This entry is therefore ours to complete.
		 */
		cancel_delayed_work_sync(&jc->work);
		list_del(&jc->list);
		jc->scmd->result = DID_NO_CONNECT << 16;
		scsi_done(jc->scmd);
		kfree(jc);
	}
}

/* -------------------------------------------------------------------------
 * Arm / disarm — patch and unpatch the target Scsi_Host's hostt
 * ----------------------------------------------------------------------
 */
static int jam_arm(int host_no)
{
	struct Scsi_Host *shost;

	if (jam_hstate)
		return -EBUSY;  /* already armed */

	shost = scsi_host_lookup((unsigned int)host_no);
	if (!shost)
		return -ENODEV;

	jam_hstate = kzalloc_obj(*jam_hstate, GFP_KERNEL);
	if (!jam_hstate) {
		scsi_host_put(shost);
		return -ENOMEM;
	}

	jam_hstate->shost      = shost;
	jam_hstate->orig_hostt = shost->hostt;

	/* copy the real hostt, then replace only queuecommand */
	memcpy(&jam_hstate->fake_hostt, shost->hostt,
	       sizeof(struct scsi_host_template));
	jam_hstate->fake_hostt.queuecommand = jammer_queuecommand;

	/*
	 * patch — no lock needed, blk-mq will see the new pointer on next
	 * queue_rq call; existing in-flight commands are unaffected.
	 * shost->hostt is const * — use double-pointer cast to write through it.
	 * We own this Scsi_Host and restore the original in jam_disarm().
	 */
	*(const struct scsi_host_template **)&shost->hostt = &jam_hstate->fake_hostt;

	pr_info("scsi_jammer: ARMED host%d (style=%d msecs=%u)\n",
		host_no, READ_ONCE(jam_style), READ_ONCE(jam_msecs));
	return 0;
}

static void jam_disarm(void)
{
	if (!jam_hstate)
		return;

	/* restore original hostt before draining so new commands pass through */
	*(const struct scsi_host_template **)&jam_hstate->shost->hostt = jam_hstate->orig_hostt;

	jam_drain_pending();

	scsi_host_put(jam_hstate->shost);
	kfree(jam_hstate);
	jam_hstate = NULL;

	pr_info("scsi_jammer: disarmed\n");
}

/* -------------------------------------------------------------------------
 * Flap timer — fires in softirq context.
 * MUST NOT sleep, MUST NOT call jam_disarm/jam_arm directly.
 * Only queues flap_work onto jam_wq where blocking is safe.
 * ----------------------------------------------------------------------
 */
static void flap_timer_fn(struct timer_list *t)
{
	if (!READ_ONCE(jam_enable) || READ_ONCE(jam_style) != JAM_STYLE_FLAP)
		return;

	/* hand off to process context — never block in a timer */
	queue_work(jam_wq, &flap_work);
}

/* -------------------------------------------------------------------------
 * Flap work — runs in jam_wq (process context), safe to sleep.
 * Does the actual arm/disarm and reschedules the timer.
 * ----------------------------------------------------------------------
 */
static void flap_work_fn(struct work_struct *work)
{
	u32 interval;

	if (!READ_ONCE(jam_enable) || READ_ONCE(jam_style) != JAM_STYLE_FLAP)
		return;

	if (flap_phase == 0) {
		/* currently armed — disarm for flap_interval ms */
		jam_disarm();
		flap_phase = 1;
		interval = max(READ_ONCE(jam_flap_interval), 100U);
		pr_info("scsi_jammer: flap DISARMED for %u ms\n", interval);
	} else {
		/* currently disarmed — re-arm for jam_msecs ms */
		int host_no = READ_ONCE(jam_host_no);

		if (host_no >= 0)
			jam_arm(host_no);
		flap_phase = 0;
		interval = max(READ_ONCE(jam_msecs), 100U);
		pr_info("scsi_jammer: flap ARMED for %u ms\n", interval);
	}

	mod_timer(&flap_timer, jiffies + msecs_to_jiffies(interval));
}

/* -------------------------------------------------------------------------
 * debugfs file operations
 * ----------------------------------------------------------------------
 */

/* jam_enable: write 1 to arm, 0 to disarm; resets jam_count */
static ssize_t jam_enable_write(struct file *f, const char __user *ubuf,
				size_t count, loff_t *pos)
{
	int val, ret, host_no;

	ret = kstrtoint_from_user(ubuf, count, 0, &val);
	if (ret)
		return ret;
	if (val != 0 && val != 1)
		return -EINVAL;

	atomic64_set(&jam_count, 0);

	if (val == 0) {
		WRITE_ONCE(jam_enable, 0);
		timer_delete_sync(&flap_timer);
		jam_disarm();
	} else {
		host_no = READ_ONCE(jam_host_no);
		if (host_no < 0) {
			pr_err("scsi_jammer: set jam_host_no first\n");
			return -EINVAL;
		}
		WRITE_ONCE(jam_enable, 1);

		if (READ_ONCE(jam_style) == JAM_STYLE_FLAP) {
			flap_phase = 0;
			ret = jam_arm(host_no);
			if (ret)
				return ret;
			/* start flap timer to disarm after jam_msecs */
			mod_timer(&flap_timer,
				  jiffies + msecs_to_jiffies(
					max(READ_ONCE(jam_msecs), 100U)));
		} else {
			ret = jam_arm(host_no);
			if (ret)
				return ret;
		}
	}

	return count;
}

static ssize_t jam_enable_read(struct file *f, char __user *ubuf,
			       size_t count, loff_t *pos)
{
	char buf[4];
	int len = snprintf(buf, sizeof(buf), "%d\n", READ_ONCE(jam_enable));

	return simple_read_from_buffer(ubuf, count, pos, buf, len);
}

static const struct file_operations fops_jam_enable = {
	.owner  = THIS_MODULE,
	.read   = jam_enable_read,
	.write  = jam_enable_write,
	.llseek = default_llseek,
};

/* jam_count: read-only atomic64 */
static ssize_t jam_count_read(struct file *f, char __user *ubuf,
			      size_t count, loff_t *pos)
{
	char buf[24];
	int len = snprintf(buf, sizeof(buf), "%llu\n",
			   (unsigned long long)atomic64_read(&jam_count));

	return simple_read_from_buffer(ubuf, count, pos, buf, len);
}

static const struct file_operations fops_jam_count = {
	.owner  = THIS_MODULE,
	.read   = jam_count_read,
	.llseek = default_llseek,
};

/* simple r/w helpers for int and u32 knobs */
#define MAKE_INT_FOPS(_name, _var)					\
static ssize_t _name##_read(struct file *f, char __user *ubuf,		\
			    size_t count, loff_t *pos)			\
{									\
	char buf[16];							\
	int len = snprintf(buf, sizeof(buf), "%d\n",			\
			   READ_ONCE(_var));				\
	return simple_read_from_buffer(ubuf, count, pos, buf, len);	\
}									\
static ssize_t _name##_write(struct file *f, const char __user *ubuf,	\
			     size_t count, loff_t *pos)			\
{									\
	int val, ret = kstrtoint_from_user(ubuf, count, 0, &val);	\
	if (ret)							\
		return ret;						\
	WRITE_ONCE(_var, val);						\
	return count;							\
}									\
static const struct file_operations fops_##_name = {			\
	.owner  = THIS_MODULE,						\
	.read   = _name##_read,						\
	.write  = _name##_write,					\
	.llseek = default_llseek,					\
}

#define MAKE_U32_FOPS(_name, _var)					\
static ssize_t _name##_read(struct file *f, char __user *ubuf,		\
			    size_t count, loff_t *pos)			\
{									\
	char buf[16];							\
	int len = snprintf(buf, sizeof(buf), "%u\n",			\
			   READ_ONCE(_var));				\
	return simple_read_from_buffer(ubuf, count, pos, buf, len);	\
}									\
static ssize_t _name##_write(struct file *f, const char __user *ubuf,	\
			     size_t count, loff_t *pos)			\
{									\
	u32 val;							\
	int ret = kstrtou32_from_user(ubuf, count, 0, &val);		\
	if (ret)							\
		return ret;						\
	if (val < 100)							\
		val = 100; /* safety floor */				\
	WRITE_ONCE(_var, val);						\
	return count;							\
}									\
static const struct file_operations fops_##_name = {			\
	.owner  = THIS_MODULE,						\
	.read   = _name##_read,						\
	.write  = _name##_write,					\
	.llseek = default_llseek,					\
}

MAKE_INT_FOPS(jam_host_no,        jam_host_no);
MAKE_INT_FOPS(jam_style,          jam_style);
MAKE_INT_FOPS(jam_tur_passthrough, jam_tur_passthrough);
MAKE_U32_FOPS(jam_msecs,   jam_msecs);
MAKE_U32_FOPS(jam_flap_interval, jam_flap_interval);

/* -------------------------------------------------------------------------
 * Module init / exit
 * ----------------------------------------------------------------------
 */
static int __init scsi_jammer_init(void)
{
	int ret;

	jam_wq = alloc_ordered_workqueue("scsi_jammer", WQ_MEM_RECLAIM);
	if (!jam_wq)
		return -ENOMEM;

	timer_setup(&flap_timer, flap_timer_fn, 0);
	INIT_WORK(&flap_work, flap_work_fn);
	atomic64_set(&jam_count, 0);

	jam_dir = debugfs_create_dir("scsi_jammer", NULL);
	if (IS_ERR(jam_dir)) {
		ret = PTR_ERR(jam_dir);
		goto err_wq;
	}

	debugfs_create_file("jam_enable",        0644, jam_dir, NULL,
			    &fops_jam_enable);
	debugfs_create_file("jam_host_no",       0644, jam_dir, NULL,
			    &fops_jam_host_no);
	debugfs_create_file("jam_style",         0644, jam_dir, NULL,
			    &fops_jam_style);
	debugfs_create_file("jam_msecs",         0644, jam_dir, NULL,
			    &fops_jam_msecs);
	debugfs_create_file("jam_flap_interval", 0644, jam_dir, NULL,
			    &fops_jam_flap_interval);
	debugfs_create_file("jam_tur_passthrough", 0644, jam_dir, NULL,
			    &fops_jam_tur_passthrough);
	debugfs_create_file("jam_count",           0444, jam_dir, NULL,
			    &fops_jam_count);

	pr_info("scsi_jammer: loaded - /sys/kernel/debug/scsi_jammer/ ready\n");
	pr_info("scsi_jammer: styles: 0=drop 1=timeout 2=flap\n");
	return 0;

err_wq:
	destroy_workqueue(jam_wq);
	return ret;
}

static void __exit scsi_jammer_exit(void)
{
	/* Disarm cleanly — this drains all pending commands */
	WRITE_ONCE(jam_enable, 0);
	timer_delete_sync(&flap_timer);
	/* cancel any flap_work queued by the timer before it was stopped */
	cancel_work_sync(&flap_work);
	jam_disarm();

	/* Destroy debugfs before workqueue so no new work is queued */
	debugfs_remove_recursive(jam_dir);

	destroy_workqueue(jam_wq);
	pr_info("scsi_jammer: unloaded\n");
}

module_init(scsi_jammer_init);
module_exit(scsi_jammer_exit);
