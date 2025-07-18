#define pr_fmt(fmt) "[%s:%d] " fmt, __func__, __LINE__

#include "linux/sched.h"
#include <linux/signal.h>
#include <linux/syscalls.h>
#include <linux/smp.h>
#include <asm/sighandling.h>
#include <asm/sigframe.h>
#include <linux/fdtable.h>
#include <linux/fs_struct.h>
#include <linux/moduleparam.h>
#include <linux/rt_ipc.h>

static bool rt_ipc_dbg = false;
core_param(rt_ipc_dbg, rt_ipc_dbg, bool, 0644);

#define WARN1(fmt, args...) ({ \
	WARN(rt_ipc_dbg, fmt, ##args); \
})

#define pr_info1(fmt, args...) { \
if (rt_ipc_dbg) \
	pr_info(fmt, ##args); \
}

static void pr_context(struct pt_regs *regs)
{
	pr_info1("di : %016lx si : %016lx bp : %016lx sp : %016lx\n", regs->di, regs->si, regs->bp, regs->sp);
	pr_info1("bx : %016lx dx : %016lx cx : %016lx ax : %016lx\n", regs->bx, regs->dx, regs->cx, regs->ax);
	pr_info1("r8 : %016lx r9 : %016lx r10: %016lx r11: %016lx\n", regs->r8, regs->r9, regs->r10, regs->r11);
	pr_info1("r12: %016lx r13: %016lx r14: %016lx r15: %016lx\n", regs->r12, regs->r13, regs->r14, regs->r15);
	pr_info1("trap_nr: %016lx error_code: %016lx ip: %016lx flag: %016lx\n",
		current->thread.trap_nr, current->thread.error_code, regs->ip, regs->flags);
	pr_info1("cs: %016x ss: %016x cr2: %016lx cr3: %016lx\n", regs->cs, regs->ss, current->thread.cr2, __read_cr3());
}

/*
 * If regs->ss will cause an IRET fault, change it.  Otherwise leave it
 * alone.  Using this generally makes no sense unless
 * user_64bit_mode(regs) would return true.
 */
static void force_valid_ss(struct pt_regs *regs)
{
	u32 ar;
	asm volatile ("lar %[old_ss], %[ar]\n\t"
		      "jz 1f\n\t"		/* If invalid: */
		      "xorl %[ar], %[ar]\n\t"	/* set ar = 0 */
		      "1:"
		      : [ar] "=r" (ar)
		      : [old_ss] "rm" ((u16)regs->ss));

	/*
	 * For a valid 64-bit user context, we need DPL 3, type
	 * read-write data or read-write exp-down data, and S and P
	 * set.  We can't use VERW because VERW doesn't check the
	 * P bit.
	 */
	ar &= AR_DPL_MASK | AR_S | AR_P | AR_TYPE_MASK;
	if (ar != (AR_DPL3 | AR_S | AR_P | AR_TYPE_RWDATA) &&
	    ar != (AR_DPL3 | AR_S | AR_P | AR_TYPE_RWDATA_EXPDOWN))
		regs->ss = __USER_DS;
}

static int x64_setup_rt_ipc_frame(struct rt_ipc_activation *activation, struct pt_regs *regs, unsigned int cmd, struct rt_ipc_info *info)
{
	struct rt_ipc_frame __user *frame;
	void __user *fp = NULL;

	frame = get_sigframe(NULL, regs, sizeof(struct rt_ipc_frame), &fp);

	if (!user_access_begin(frame, sizeof(*frame))) {
		pr_err("%s:%d handler: 0x%lx\n", __func__, __LINE__, (unsigned long)frame);
		return -EFAULT;
	}

	/* Set up to return from userspace.  If provided, use a stub
	   already in userspace.  */
	unsafe_put_user(activation->act->restorer, &frame->pretcode, Efault);
	user_access_end();

	if (copy_to_user(&frame->info, info , sizeof(struct rt_ipc_info)))
		return -EFAULT;

	pr_info1("&frame->info: %llx", (u64)(&frame->info));
	/* TODO: process frame->info */

	regs->di = cmd;
	regs->ax = 0;

	regs->si = (unsigned long)&frame->info;

	regs->ip = (unsigned long)activation->act->entry;

	regs->sp = (unsigned long)frame;

	regs->cs = __USER_CS;

	if (unlikely(regs->ss != __USER_DS))
		force_valid_ss(regs);

	return 0;

Efault:
	user_access_end();
	return -EFAULT;
}

static void save_context(struct rt_ipc_context *ctx, struct pt_regs *regs)
{
	ctx->regs = *regs;
	ctx->trap_nr = current->thread.trap_nr;
	ctx->error_code = current->thread.error_code;
	ctx->gs = 0;
	ctx->fs = 0;
	ctx->cr2 = current->thread.cr2;
	/* TODO: fpstate save restore */
}

static void restore_context(struct rt_ipc_context *ctx, struct pt_regs *regs)
{
	current->restart_block.fn = do_no_restart_syscall;

	ctx->regs.flags = (regs->flags & ~FIX_EFLAGS) | (ctx->regs.flags & FIX_EFLAGS);

	*regs = ctx->regs;

	regs->cs |= 0x03;
	regs->ss |= 0x03;
	regs->orig_ax = -1;
}

int rt_ipc_config_activation(struct task_struct *task)
{
	struct pt_regs *regs = task_pt_regs(task);
	struct rt_ipc_activation *activation;

	list_for_each_entry(activation, &task->rt_ipc_activation_free, activation_link) {
		save_context(&activation->server_ctx, regs);

		pr_info("activation pid: %d stack: %016lx\n", activation->s->pid, activation->stack);
	}

	pr_context(regs);

	WARN1("%s:%d\n", __func__, __LINE__);
	pr_info("%s:%d pid: %d\n", __func__, __LINE__, current->pid);

	return 0;
}

static void save_task_context(struct task_struct *p, struct rt_ipc_activation *act)
{
	act->context.files = p->files;
	act->context.fs = p->fs;
	act->context.sighand = p->sighand;
	act->context.signal = p->signal;
	act->context.thread_pid = p->thread_pid;
	act->context.pid = p->pid;
	act->context.tgid = p->tgid;
	act->context.rseq = p->rseq;
	act->context.rseq_sig = p->rseq_sig;
	act->context.rseq_event_mask = p->rseq_event_mask;
	act->context.mm = p->mm;
	act->context.active_mm = p->active_mm;
	act->context.nsproxy = p->nsproxy;
	act->context.min_flt = p->min_flt;
	act->context.maj_flt = p->maj_flt;
	act->context.fsbase = p->thread.fsbase;
	act->context.group_leader = p->group_leader;
}

#define CONFIG_SWITCH_THREAD_GROUP 1

static void migrate_thread(struct task_struct *c, struct task_struct *s, struct rt_ipc_activation *act)
{
	save_task_context(c, act);

	atomic_inc(&s->files->count);
	c->files = s->files;

	c->fs->users++;
	c->fs = s->fs;

	c->rseq = s->rseq;
	c->rseq_sig = s->rseq_sig;
	c->rseq_event_mask = s->rseq_event_mask;

	atomic_inc(&s->mm->mm_users);
	c->mm = s->mm;
	c->active_mm = s->mm;

	c->nsproxy = s->nsproxy;

	c->min_flt = s->min_flt;
	c->maj_flt = s->maj_flt;

	/* fsbase register for glibc tls, thread local storage */
	c->thread.fsbase = s->thread.fsbase;
	if (static_cpu_has(X86_FEATURE_FSGSBASE)) {
		wrfsbase(c->thread.fsbase);
	}
}

static void restore_thread_from_upcall(struct task_struct *c, struct task_struct *s, struct rt_ipc_activation *act)
{
	struct rt_ipc_migrate_context *ctx = &act->context;

	c->files = ctx->files;
	atomic_dec(&s->files->count);

	c->fs = ctx->fs;
	s->fs->users--;

	c->rseq = ctx->rseq;
	c->rseq_sig = ctx->rseq_sig;
	c->rseq_event_mask = ctx->rseq_event_mask;

	c->mm = ctx->mm;
	c->active_mm = ctx->active_mm;
	atomic_dec(&s->mm->mm_users);

	c->nsproxy = ctx->nsproxy;

	c->min_flt = ctx->min_flt;
	c->maj_flt = ctx->maj_flt;

	c->thread.fsbase = ctx->fsbase;
	if (static_cpu_has(X86_FEATURE_FSGSBASE)) {
		wrfsbase(c->thread.fsbase);
	}
}

static void upcall_setup(struct rt_ipc_activation *activation, unsigned int cmd, struct rt_ipc_info *info)
{
	struct pt_regs *regs = current_pt_regs();
	unsigned long flags;
	spinlock_t *lock = &activation->s->sighand->siglock;
	pr_info1("c->fsbase: %016lx s->fsbase: %016lx\n", activation->c->thread.fsbase, activation->s->thread.fsbase);
	pr_context(regs);
	pr_info1("pt_regs: %px\n", regs);
	WARN1("%s:%d s->mm->mm_users: %d READ_ONCE(c->__state): %d READ_ONCE(s->__state): %d\n",
		__func__, __LINE__, atomic_read(&activation->s->mm->mm_users), READ_ONCE(activation->c->__state), READ_ONCE(activation->s->__state));

	save_context(&activation->client_ctx, regs);

	spin_lock_irqsave(lock, flags);

	migrate_thread(activation->c, activation->s, activation);

	rt_ipc_context_switch(activation->c);
	spin_unlock_irqrestore(lock, flags);

	restore_context(&activation->server_ctx, regs);

	regs->sp = activation->stack;

	if (x64_setup_rt_ipc_frame(activation, regs, cmd, info) < 0) {
		pr_err("setup rt ipc frame failed\n");
		goto out;
	}

	pr_context(regs);

	pr_info1("c->fsbase: %016lx s->fsbase: %016lx\n", activation->c->thread.fsbase, activation->s->thread.fsbase);
	WARN1("%s:%d\n", __func__, __LINE__);
	pr_info1("%s:%d handler: %016lx %016lx\n", __func__, __LINE__, (size_t)activation->act->entry, (size_t)activation->act->restorer);

	return;

out:
	signal_fault(regs, activation, "rt_ipc_migrate_thread");
}

int rt_ipc_migrate_thread(struct rt_ipc_activation *activation, unsigned int cmd, struct rt_ipc_info *info)
{
	activation->c = current;

	upcall_setup(activation, cmd, info);

	list_add(&activation->activation_link, &current->rt_ipc_activation_in_use);

	return 0;
}

static void clear_activation_info(struct rt_ipc_activation *activation)
{
	activation->c = NULL;
}

static int rt_ipc_return(struct pt_regs *regs, struct rt_ipc_info *info)
{
	unsigned long flags;
	spinlock_t *sighand_lock;
	spinlock_t *rt_ipc_lock;

	struct rt_ipc_activation *activation =
		list_first_entry_or_null(&current->rt_ipc_activation_in_use, struct rt_ipc_activation, activation_link);
	if (!activation) {
		pr_err("invaild activation\n");
		return -EFAULT;
	}

	sighand_lock = &activation->s->sighand->siglock;
	rt_ipc_lock = &activation->s->group_leader->rt_ipc_lock;

	spin_lock_irqsave(sighand_lock, flags);

	restore_thread_from_upcall(activation->c, activation->s, activation);
	rt_ipc_context_switch(activation->c);

	spin_unlock_irqrestore(sighand_lock, flags);

	restore_context(&activation->client_ctx, regs);

	spin_lock_irqsave(rt_ipc_lock, flags);
	clear_activation_info(activation);
	list_move(&activation->activation_link, &activation->s->group_leader->rt_ipc_activation_free);
	spin_unlock_irqrestore(rt_ipc_lock, flags);

	pr_info1("READ_ONCE(c->__state): %d READ_ONCE(s->__state): %d current: %d\n",
		READ_ONCE(current->__state), READ_ONCE(activation->s->__state), READ_ONCE(current->__state));
	WARN1("%s:%d\n", __func__, __LINE__);
	pr_info1("c->fsbase: %016lx s->fsbase: %016lx\n", current->thread.fsbase, activation->s->thread.fsbase);
	pr_context(regs);

	if (copy_to_user(activation->info, info , sizeof(struct rt_ipc_info))) {
		pr_err("activation->info: %lx info: %lx", (size_t)activation->info, (size_t)info);
		return -EFAULT;
	}
	return 0;
}

SYSCALL_DEFINE0(rt_ipc_return)
{
	struct pt_regs *regs = current_pt_regs();
	struct rt_ipc_frame __user *frame;
	struct rt_ipc_info info;

	frame = (struct rt_ipc_frame __user *)(regs->sp - sizeof(long));
	if (!access_ok(frame, sizeof(*frame)))
		goto badframe;

	pr_info1("&frame->info: %lx regs->si: %lx", (size_t)(&frame->info), regs->si);

	if (copy_from_user(&info, &frame->info, sizeof(info))) {
		goto badframe;
	}
	/* TODO: process frame->info */

	if (rt_ipc_return(regs, &info) < 0) {
		goto badframe;
	}

	regs->ax = 0;

	return regs->ax;

badframe:
	signal_fault(regs, frame, "rt_ipc_return");
	return 0;
}
