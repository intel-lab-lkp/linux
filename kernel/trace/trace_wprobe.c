// SPDX-License-Identifier: GPL-2.0
/*
 * Hardware-breakpoint-based tracing events
 *
 * Copyright (C) 2023, Masami Hiramatsu <mhiramat@kernel.org>
 */
#define pr_fmt(fmt)	"trace_wprobe: " fmt

#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/hw_breakpoint.h>
#include <linux/kallsyms.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/perf_event.h>
#include <linux/rculist.h>
#include <linux/security.h>
#include <linux/spinlock.h>
#include <linux/tracepoint.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/irq_work.h>
#include <linux/preempt.h>

#include <asm/ptrace.h>

#include "trace.h"
#include "trace_btf.h"
#include "trace_dynevent.h"
#include "trace_probe.h"
#include "trace_probe_kernel.h"
#include "trace_probe_tmpl.h"
#include "trace_output.h"

#define WPROBE_EVENT_SYSTEM "wprobes"

static int trace_wprobe_create(const char *raw_command);
static int trace_wprobe_show(struct seq_file *m, struct dyn_event *ev);
static int trace_wprobe_release(struct dyn_event *ev);
static bool trace_wprobe_is_busy(struct dyn_event *ev);
static bool trace_wprobe_match(const char *system, const char *event,
			       int argc, const char **argv, struct dyn_event *ev);

static struct dyn_event_operations trace_wprobe_ops = {
	.create = trace_wprobe_create,
	.show = trace_wprobe_show,
	.is_busy = trace_wprobe_is_busy,
	.free = trace_wprobe_release,
	.match = trace_wprobe_match,
};

struct trace_wprobe {
	struct dyn_event	devent;
	struct perf_event * __percpu *bp_event;
	unsigned long		addr;
	int			len;
	int			type;
	const char		*symbol;
	raw_spinlock_t		lock;
	struct irq_work		irq_work;
	struct work_struct	work;
	atomic_t		missed;
	struct trace_probe	tp;
};

static bool is_trace_wprobe(struct dyn_event *ev)
{
	return ev->ops == &trace_wprobe_ops;
}

static struct trace_wprobe *to_trace_wprobe(struct dyn_event *ev)
{
	return container_of(ev, struct trace_wprobe, devent);
}

#define for_each_trace_wprobe(pos, dpos)			\
	for_each_dyn_event(dpos)				\
		if (is_trace_wprobe(dpos) && (pos = to_trace_wprobe(dpos)))

static bool trace_wprobe_is_busy(struct dyn_event *ev)
{
	struct trace_wprobe *tw = to_trace_wprobe(ev);

	return trace_probe_is_enabled(&tw->tp);
}

static bool trace_wprobe_match(const char *system, const char *event,
			       int argc, const char **argv, struct dyn_event *ev)
{
	struct trace_wprobe *tw = to_trace_wprobe(ev);

	if (event[0] != '\0' && strcmp(trace_probe_name(&tw->tp), event))
		return false;

	if (system && strcmp(trace_probe_group_name(&tw->tp), system))
		return false;

	return trace_probe_match_command_args(&tw->tp, argc, argv);
}

/*
 * Note that we don't verify the fetch_insn code, since it does not come
 * from user space.
 */
static int
process_fetch_insn(struct fetch_insn *code, void *rec, void *edata,
		   void *dest, void *base)
{
	void *baddr = rec;
	unsigned long val;
	int ret;

retry:
	/* 1st stage: get value from context */
	switch (code->op) {
	case FETCH_OP_BADDR:
		val = (unsigned long)baddr;
		break;
	case FETCH_NOP_SYMBOL:	/* Ignore a place holder */
		code++;
		goto retry;
	default:
		ret = process_common_fetch_insn(code, &val);
		if (ret < 0)
			return ret;
	}
	code++;

	return process_fetch_insn_bottom(code, val, dest, base);
}
NOKPROBE_SYMBOL(process_fetch_insn)

static void wprobe_trace_handler(struct trace_wprobe *tw,
				 unsigned long addr,
				 struct pt_regs *regs,
				 struct trace_event_file *trace_file)
{
	struct wprobe_trace_entry_head *entry;
	struct trace_event_call *call = trace_probe_event_call(&tw->tp);
	struct trace_event_buffer fbuffer;
	int dsize;

	if (WARN_ON_ONCE(call != trace_file->event_call))
		return;

	if (trace_trigger_soft_disabled(trace_file))
		return;

	if (READ_ONCE(tw->addr) != addr)
		return;

	dsize = __get_data_size(&tw->tp, (void *)addr, NULL);

	entry = trace_event_buffer_reserve(&fbuffer, trace_file,
					   sizeof(*entry) + tw->tp.size + dsize);
	if (!entry)
		return;

	entry->ip = instruction_pointer(regs);
	store_trace_args(&entry[1], &tw->tp, (void *)addr, NULL, sizeof(*entry), dsize);

	fbuffer.regs = regs;
	trace_event_buffer_commit(&fbuffer);
}

static void wprobe_perf_handler(struct perf_event *bp,
			      struct perf_sample_data *data,
			      struct pt_regs *regs)
{
	struct trace_wprobe *tw = bp->overflow_handler_context;
	struct event_file_link *link;
	unsigned long addr = bp->attr.bp_addr;

	trace_probe_for_each_link_rcu(link, &tw->tp)
		wprobe_trace_handler(tw, addr, regs, link->file);
}

static int __register_trace_wprobe(struct trace_wprobe *tw)
{
	struct perf_event_attr attr;
	int i, ret;

	if (tw->bp_event)
		return -EINVAL;

	for (i = 0; i < tw->tp.nr_args; i++) {
		ret = traceprobe_update_arg(&tw->tp.args[i]);
		if (ret)
			return ret;
	}

	hw_breakpoint_init(&attr);
	attr.bp_addr = tw->addr;
	attr.bp_len = tw->len;
	attr.bp_type = tw->type;

	tw->bp_event = register_wide_hw_breakpoint(&attr, wprobe_perf_handler, tw);
	if (IS_ERR_PCPU(tw->bp_event)) {
		int ret = PTR_ERR_PCPU(tw->bp_event);

		tw->bp_event = NULL;
		return ret;
	}

	return 0;
}

static void __unregister_trace_wprobe(struct trace_wprobe *tw)
{
	if (tw->bp_event) {
		irq_work_sync(&tw->irq_work);
		cancel_work_sync(&tw->work);
		unregister_wide_hw_breakpoint(tw->bp_event);
		tw->bp_event = NULL;
	}
}

static int trace_wprobe_update_local(struct trace_wprobe *tw, unsigned long addr)
{
	struct perf_event *bp = *this_cpu_ptr(tw->bp_event);
	struct perf_event_attr attr = bp->attr;

	attr.bp_addr = addr;
	return modify_wide_hw_breakpoint_local(bp, &attr);
}

static void wprobe_smp_update_func(void *info)
{
	struct trace_wprobe *tw = info;
	unsigned long addr = READ_ONCE(tw->addr);

	trace_wprobe_update_local(tw, addr);
}

static void wprobe_work_func(struct work_struct *work)
{
	struct trace_wprobe *tw = container_of(work, struct trace_wprobe, work);

	on_each_cpu(wprobe_smp_update_func, tw, true);
}

static void wprobe_irq_work_func(struct irq_work *irq_work)
{
	struct trace_wprobe *tw = container_of(irq_work, struct trace_wprobe, irq_work);

	schedule_work(&tw->work);
}

static void free_trace_wprobe(struct trace_wprobe *tw)
{
	if (tw) {
		irq_work_sync(&tw->irq_work);
		cancel_work_sync(&tw->work);
		trace_probe_cleanup(&tw->tp);
		kfree(tw->symbol);
		kfree(tw);
	}
}
DEFINE_FREE(free_trace_wprobe, struct trace_wprobe *,
	if (!IS_ERR_OR_NULL(_T))
		free_trace_wprobe(_T))


static struct trace_wprobe *alloc_trace_wprobe(const char *group,
					       const char *event,
					       const char *symbol,
					       unsigned long addr,
					       int len, int type, int nargs)
{
	struct trace_wprobe *tw __free(free_trace_wprobe) = NULL;
	int ret;

	tw = kzalloc_flex(*tw, tp.args, nargs);
	if (!tw)
		return ERR_PTR(-ENOMEM);

	if (symbol) {
		tw->symbol = kstrdup(symbol, GFP_KERNEL);
		if (!tw->symbol)
			return ERR_PTR(-ENOMEM);
	}
	tw->addr = addr;
	tw->len = len;
	tw->type = type;
	raw_spin_lock_init(&tw->lock);
	init_irq_work(&tw->irq_work, wprobe_irq_work_func);
	INIT_WORK(&tw->work, wprobe_work_func);
	atomic_set(&tw->missed, 0);

	ret = trace_probe_init(&tw->tp, event, group, false, nargs);
	if (ret < 0)
		return ERR_PTR(ret);

	dyn_event_init(&tw->devent, &trace_wprobe_ops);
	return_ptr(tw);
}

static struct trace_wprobe *find_trace_wprobe(const char *event,
					      const char *group)
{
	struct dyn_event *pos;
	struct trace_wprobe *tw;

	for_each_trace_wprobe(tw, pos)
		if (strcmp(trace_probe_name(&tw->tp), event) == 0 &&
		    strcmp(trace_probe_group_name(&tw->tp), group) == 0)
			return tw;
	return NULL;
}

static enum print_line_t
print_wprobe_event(struct trace_iterator *iter, int flags,
		   struct trace_event *event)
{
	struct wprobe_trace_entry_head *field;
	struct trace_seq *s = &iter->seq;
	struct trace_probe *tp;

	field = (struct wprobe_trace_entry_head *)iter->ent;
	tp = trace_probe_primary_from_call(
		container_of(event, struct trace_event_call, event));
	if (WARN_ON_ONCE(!tp))
		goto out;

	trace_seq_printf(s, "%s: (", trace_probe_name(tp));

	if (!seq_print_ip_sym_offset(s, field->ip, flags))
		goto out;

	trace_seq_putc(s, ')');

	if (trace_probe_print_args(s, tp->args, tp->nr_args,
			     (u8 *)&field[1], field) < 0)
		goto out;

	trace_seq_putc(s, '\n');
out:
	return trace_handle_return(s);
}

static int wprobe_event_define_fields(struct trace_event_call *event_call)
{
	int ret;
	struct wprobe_trace_entry_head field;
	struct trace_probe *tp;

	tp = trace_probe_primary_from_call(event_call);
	if (WARN_ON_ONCE(!tp))
		return -ENOENT;

	DEFINE_FIELD(unsigned long, ip, FIELD_STRING_IP, 0);

	return traceprobe_define_arg_fields(event_call, sizeof(field), tp);
}

static struct trace_event_functions wprobe_funcs = {
	.trace	= print_wprobe_event
};

static struct trace_event_fields wprobe_fields_array[] = {
	{ .type = TRACE_FUNCTION_TYPE,
	  .define_fields = wprobe_event_define_fields },
	{}
};

static int wprobe_register(struct trace_event_call *event,
			   enum trace_reg type, void *data);

static inline void init_trace_event_call(struct trace_wprobe *tw)
{
	struct trace_event_call *call = trace_probe_event_call(&tw->tp);

	call->event.funcs = &wprobe_funcs;
	call->class->fields_array = wprobe_fields_array;
	call->flags = TRACE_EVENT_FL_WPROBE;
	call->class->reg = wprobe_register;
}

static int register_wprobe_event(struct trace_wprobe *tw)
{
	init_trace_event_call(tw);
	return trace_probe_register_event_call(&tw->tp);
}

static int register_trace_wprobe_event(struct trace_wprobe *tw)
{
	struct trace_wprobe *old_tw;
	int ret;

	guard(mutex)(&event_mutex);

	old_tw = find_trace_wprobe(trace_probe_name(&tw->tp),
				   trace_probe_group_name(&tw->tp));
	if (old_tw) {
		/*
		 * Wprobe does not support sibling probes because the event
		 * trigger (set_wprobe/clear_wprobe) identifies the target
		 * wprobe by its event name. Having multiple wprobes sharing
		 * the same event name would make the target ambiguous.
		 */
		trace_probe_log_set_index(0);
		trace_probe_log_err(0, WPROBE_NO_SIBLING);
		return -EBUSY;
	}

	ret = register_wprobe_event(tw);
	if (ret)
		return ret;

	dyn_event_add(&tw->devent, trace_probe_event_call(&tw->tp));
	return 0;
}
static int unregister_wprobe_event(struct trace_wprobe *tw)
{
	return trace_probe_unregister_event_call(&tw->tp);
}

static int unregister_trace_wprobe(struct trace_wprobe *tw)
{
	if (trace_probe_has_sibling(&tw->tp))
		goto unreg;

	if (trace_probe_is_enabled(&tw->tp))
		return -EBUSY;

	if (trace_event_dyn_busy(trace_probe_event_call(&tw->tp)))
		return -EBUSY;

	if (unregister_wprobe_event(tw))
		return -EBUSY;

unreg:
	__unregister_trace_wprobe(tw);
	dyn_event_remove(&tw->devent);
	trace_probe_unlink(&tw->tp);

	return 0;
}

static int enable_trace_wprobe(struct trace_event_call *call,
			       struct trace_event_file *file)
{
	struct trace_probe *tp;
	struct trace_wprobe *tw;
	bool enabled;
	int ret = 0;

	tp = trace_probe_primary_from_call(call);
	if (WARN_ON_ONCE(!tp))
		return -ENODEV;
	enabled = trace_probe_is_enabled(tp);

	if (file) {
		ret = trace_probe_add_file(tp, file);
		if (ret)
			return ret;
	} else {
		trace_probe_set_flag(tp, TP_FLAG_PROFILE);
	}

	if (!enabled) {
		list_for_each_entry(tw, trace_probe_probe_list(tp), tp.list) {
			ret = __register_trace_wprobe(tw);
			if (ret < 0) {
				struct trace_wprobe *tmp;

				list_for_each_entry(tmp, trace_probe_probe_list(tp), tp.list) {
					if (tmp == tw)
						break;
					__unregister_trace_wprobe(tmp);
				}
				if (file)
					trace_probe_remove_file(tp, file);
				else
					trace_probe_clear_flag(tp, TP_FLAG_PROFILE);
				return ret;
			}
		}
	}

	return 0;
}

static int disable_trace_wprobe(struct trace_event_call *call,
				struct trace_event_file *file)
{
	struct trace_wprobe *tw;
	struct trace_probe *tp;

	tp = trace_probe_primary_from_call(call);
	if (WARN_ON_ONCE(!tp))
		return -ENODEV;

	if (file) {
		if (!trace_probe_get_file_link(tp, file))
			return -ENOENT;
		if (!trace_probe_has_single_file(tp))
			goto out;
		trace_probe_clear_flag(tp, TP_FLAG_TRACE);
	} else {
		trace_probe_clear_flag(tp, TP_FLAG_PROFILE);
	}

	if (!trace_probe_is_enabled(tp)) {
		list_for_each_entry(tw, trace_probe_probe_list(tp), tp.list) {
			__unregister_trace_wprobe(tw);
		}
	}

out:
	if (file)
		trace_probe_remove_file(tp, file);

	return 0;
}

static int wprobe_register(struct trace_event_call *event,
			   enum trace_reg type, void *data)
{
	struct trace_event_file *file = data;

	switch (type) {
	case TRACE_REG_REGISTER:
		return enable_trace_wprobe(event, file);
	case TRACE_REG_UNREGISTER:
		return disable_trace_wprobe(event, file);

#ifdef CONFIG_PERF_EVENTS
	case TRACE_REG_PERF_REGISTER:
	case TRACE_REG_PERF_UNREGISTER:
	case TRACE_REG_PERF_OPEN:
	case TRACE_REG_PERF_CLOSE:
	case TRACE_REG_PERF_ADD:
	case TRACE_REG_PERF_DEL:
		return -EOPNOTSUPP;
#endif
	}
	return 0;
}

static int parse_address_spec(const char *spec, unsigned long *addr, int *type,
			      int *len, char **symbol)
{
	char *_spec __free(kfree) = NULL;
	int _len = HW_BREAKPOINT_LEN_4;
	int _type = HW_BREAKPOINT_RW;
	unsigned long _addr = 0;
	char *at, *col;

	_spec = kstrdup(spec, GFP_KERNEL);
	if (!_spec)
		return -ENOMEM;

	at = strchr(_spec, '@');
	col = strchr(_spec, ':');

	if (!at) {
		trace_probe_log_err(0, BAD_ACCESS_FMT);
		return -EINVAL;
	}

	if (at != _spec) {
		*at = '\0';

		if (strcmp(_spec, "r") == 0)
			_type = HW_BREAKPOINT_R;
		else if (strcmp(_spec, "w") == 0)
			_type = HW_BREAKPOINT_W;
		else if (strcmp(_spec, "rw") == 0)
			_type = HW_BREAKPOINT_RW;
		else {
			trace_probe_log_err(0, BAD_ACCESS_TYPE);
			return -EINVAL;
		}
	}

	if (col) {
		*col = '\0';
		if (kstrtoint(col + 1, 0, &_len)) {
			trace_probe_log_err(col + 1 - _spec, BAD_ACCESS_LEN);
			return -EINVAL;
		}

		switch (_len) {
		case 1:
			_len = HW_BREAKPOINT_LEN_1;
			break;
		case 2:
			_len = HW_BREAKPOINT_LEN_2;
			break;
		case 4:
			_len = HW_BREAKPOINT_LEN_4;
			break;
		case 8:
			_len = HW_BREAKPOINT_LEN_8;
			break;
		default:
			trace_probe_log_err(col + 1 - _spec, BAD_ACCESS_LEN);
			return -EINVAL;
		}
	}

	if (kstrtoul(at + 1, 0, &_addr) != 0) {
		char *off_str = strpbrk(at + 1, "+-");
		int offset = 0;

		if (off_str) {
			if (kstrtoint(off_str, 0, &offset) != 0) {
				trace_probe_log_err(off_str - _spec, BAD_PROBE_ADDR);
				return -EINVAL;
			}
			*off_str = '\0';
		}
		_addr = kallsyms_lookup_name(at + 1);
		if (!_addr) {
			trace_probe_log_err(at + 1 - _spec, BAD_ACCESS_ADDR);
			return -ENOENT;
		}
		_addr += offset;
		*symbol = kstrdup(at + 1, GFP_KERNEL);
		if (!*symbol)
			return -ENOMEM;
	}

	*addr = _addr;
	*type = _type;
	*len = _len;
	return 0;
}

static int __trace_wprobe_create(int argc, const char *argv[])
{
	/*
	 * Argument syntax:
	 *  b[:[GRP/][EVENT]] SPEC
	 *
	 * SPEC:
	 *  [r|w|rw]@[ADDR|SYMBOL[+OFFS]][:LEN]
	 */
	struct traceprobe_parse_context *ctx __free(traceprobe_parse_context) = NULL;
	struct trace_wprobe *tw __free(free_trace_wprobe) = NULL;
	const char *event = NULL, *group = WPROBE_EVENT_SYSTEM;
	const char *tplog __free(trace_probe_log_clear) = NULL;
	char *symbol __free(kfree) = NULL;
	char *gbuf __free(kfree) = NULL;
	char *ebuf __free(kfree) = NULL;
	unsigned long addr;
	int len, type, i;
	int ret = 0;

	if (argv[0][0] != 'w')
		return -ECANCELED;

	if (argc < 2)
		return -EINVAL;

	tplog = trace_probe_log_init("wprobe", argc, argv);

	if (argv[0][1] != '\0') {
		if (argv[0][1] != ':') {
			trace_probe_log_set_index(0);
			trace_probe_log_err(1, BAD_MAXACT_TYPE);
			return -EINVAL;
		}
		event = &argv[0][2];
	}

	trace_probe_log_set_index(1);
	ret = parse_address_spec(argv[1], &addr, &type, &len, &symbol);
	if (ret < 0)
		return ret;

	trace_probe_log_set_index(0);
	if (event) {
		gbuf = kmalloc(MAX_EVENT_NAME_LEN, GFP_KERNEL);
		if (!gbuf)
			return -ENOMEM;
		ret = traceprobe_parse_event_name(&event, &group, gbuf,
						  event - argv[0]);
		if (ret)
			return ret;
	}

	if (!event) {
		/* Make a new event name */
		ebuf = kmalloc(MAX_EVENT_NAME_LEN, GFP_KERNEL);
		if (!ebuf)
			return -ENOMEM;
		if (symbol)
			snprintf(ebuf, MAX_EVENT_NAME_LEN, "%s", symbol);
		else
			snprintf(ebuf, MAX_EVENT_NAME_LEN, "w_0x%lx", addr);
		sanitize_event_name(ebuf);
		event = ebuf;
	}

	argc -= 2; argv += 2;
	tw = alloc_trace_wprobe(group, event, symbol, addr, len, type, argc);
	if (IS_ERR(tw))
		return PTR_ERR(tw);

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;

	ctx->flags = TPARG_FL_KERNEL | TPARG_FL_WPROBE;

	/* parse arguments */
	for (i = 0; i < argc; i++) {
		trace_probe_log_set_index(i + 2);
		ctx->offset = 0;
		ret = traceprobe_parse_probe_arg(&tw->tp, i, argv[i], ctx);
		if (ret)
			return ret;	/* This can be -ENOMEM */
	}

	ret = traceprobe_set_print_fmt(&tw->tp, PROBE_PRINT_NORMAL);
	if (ret < 0)
		return ret;

	ret = register_trace_wprobe_event(tw);
	if (!ret)
		tw = NULL; /* To avoid free */

	return ret;
}

static int trace_wprobe_create(const char *raw_command)
{
	return trace_probe_create(raw_command, __trace_wprobe_create);
}

static int trace_wprobe_release(struct dyn_event *ev)
{
	struct trace_wprobe *tw = to_trace_wprobe(ev);
	int ret = unregister_trace_wprobe(tw);

	if (!ret)
		free_trace_wprobe(tw);
	return ret;
}

static int trace_wprobe_show(struct seq_file *m, struct dyn_event *ev)
{
	struct trace_wprobe *tw = to_trace_wprobe(ev);
	int i;

	seq_printf(m, "w:%s/%s", trace_probe_group_name(&tw->tp),
		   trace_probe_name(&tw->tp));

	const char *type_str;

	if (tw->type == HW_BREAKPOINT_R)
		type_str = "r";
	else if (tw->type == HW_BREAKPOINT_W)
		type_str = "w";
	else
		type_str = "rw";

	int len;

	if (tw->len == HW_BREAKPOINT_LEN_1)
		len = 1;
	else if (tw->len == HW_BREAKPOINT_LEN_2)
		len = 2;
	else if (tw->len == HW_BREAKPOINT_LEN_4)
		len = 4;
	else
		len = 8;

	if (tw->symbol) {
		unsigned long sym_addr = kallsyms_lookup_name(tw->symbol);
		long offset = sym_addr ? (long)(tw->addr - sym_addr) : 0;

		if (offset)
			seq_printf(m, " %s@%s%+ld:%d", type_str, tw->symbol, offset, len);
		else
			seq_printf(m, " %s@%s:%d", type_str, tw->symbol, len);
	} else {
		seq_printf(m, " %s@0x%lx:%d", type_str, tw->addr, len);
	}

	for (i = 0; i < tw->tp.nr_args; i++)
		seq_printf(m, " %s=%s", tw->tp.args[i].name, tw->tp.args[i].comm);
	seq_putc(m, '\n');

	return 0;
}

static __init int init_wprobe_trace(void)
{
	return dyn_event_register(&trace_wprobe_ops);
}
fs_initcall(init_wprobe_trace);

#ifdef CONFIG_WPROBE_TRIGGERS

static int wprobe_trigger_global_enabled;

#define SET_WPROBE_STR		"set_wprobe"
#define CLEAR_WPROBE_STR	"clear_wprobe"
#define WPROBE_DEFAULT_CLEAR_ADDRESS ((unsigned long)&wprobe_trigger_global_enabled)

struct wprobe_trigger_data {
	struct rcu_head rcu;
	struct trace_event_file *file;
	struct trace_wprobe *tw;
	unsigned int		offset;
	long			adjust;
	const char		*field;
	bool			clear;
};

static void wprobe_trigger(struct event_trigger_data *data,
			   struct trace_buffer *buffer,  void *rec,
			   struct ring_buffer_event *event)
{
	struct wprobe_trigger_data *wprobe_data = data->private_data;
	struct trace_wprobe *tw = wprobe_data->tw;
	unsigned long addr, flags;
	bool changed = false;

	addr = *(unsigned long *)(rec + wprobe_data->offset);
	addr += wprobe_data->adjust;

	if (in_nmi()) {
		atomic_inc(&tw->missed);
		return;
	}

	raw_spin_lock_irqsave(&tw->lock, flags);

	if (!wprobe_data->clear) {
		if (tw->addr == WPROBE_DEFAULT_CLEAR_ADDRESS) {
			tw->addr = addr;
			changed = true;
			clear_bit(EVENT_FILE_FL_SOFT_DISABLED_BIT, &wprobe_data->file->flags);
		}
	} else {
		if (tw->addr != WPROBE_DEFAULT_CLEAR_ADDRESS) {
			if (!wprobe_data->field || tw->addr == addr) {
				tw->addr = WPROBE_DEFAULT_CLEAR_ADDRESS;
				changed = true;
				set_bit(EVENT_FILE_FL_SOFT_DISABLED_BIT, &wprobe_data->file->flags);
			}
		}
	}

	if (changed)
		irq_work_queue(&tw->irq_work);

	raw_spin_unlock_irqrestore(&tw->lock, flags);
}

static void free_wprobe_trigger_data(struct wprobe_trigger_data *wprobe_data)
{
	if (wprobe_data) {
		kfree(wprobe_data->field);
		kfree(wprobe_data);
	}
}
DEFINE_FREE(free_wprobe_trigger_data, struct wprobe_trigger_data *, free_wprobe_trigger_data(_T));

static void free_private_wprobe_trigger_data(void *data)
{
	free_wprobe_trigger_data(data);
}

static int wprobe_trigger_print(struct seq_file *m,
			       struct event_trigger_data *data)
{
	struct wprobe_trigger_data *wprobe_data = data->private_data;

	if (wprobe_data->clear) {
		seq_printf(m, "%s:%s", CLEAR_WPROBE_STR,
			   trace_event_name(wprobe_data->file->event_call));
		if (wprobe_data->field) {
			seq_printf(m, ":%s%+ld",
				   wprobe_data->field, wprobe_data->adjust);
		}
	} else
		seq_printf(m, "%s:%s:%s%+ld", SET_WPROBE_STR,
			   trace_event_name(wprobe_data->file->event_call),
			   wprobe_data->field, wprobe_data->adjust);

	if (data->filter_str)
		seq_printf(m, " if %s\n", data->filter_str);
	else
		seq_putc(m, '\n');

	return 0;
}

static struct wprobe_trigger_data *
wprobe_trigger_alloc(struct trace_wprobe *tw, struct trace_event_file *file,
		     bool clear)
{
	struct wprobe_trigger_data *wprobe_data;

	wprobe_data = kzalloc_obj(*wprobe_data);
	if (!wprobe_data)
		return NULL;

	wprobe_data->tw = tw;
	wprobe_data->clear = clear;
	wprobe_data->file = file;

	return wprobe_data;
}

static void wprobe_trigger_free(struct event_trigger_data *data)
{
	struct wprobe_trigger_data *wprobe_data = data->private_data;

	if (WARN_ON_ONCE(data->ref <= 0))
		return;

	data->ref--;
	if (!data->ref) {
		/* Remove the SOFT_MODE flag */
		trace_event_enable_disable(wprobe_data->file, 0, 1);
		trace_event_put_ref(wprobe_data->file->event_call);
		trigger_data_free(data);
	}
}

#ifdef CONFIG_PROBE_EVENTS_BTF_ARGS

static int get_offset_of_field(struct btf *btf, const struct btf_type *type, char *field_name)
{
	const struct btf_member *field;
	int bitoffs = 0;
	u32 anon_offs;
	char *next;

	do {
		next = strchr(field_name, '.');
		if (next)
			*next++ = '\0';

		field = btf_find_struct_member(btf, type, field_name, &anon_offs);
		if (IS_ERR_OR_NULL(field))
			return -ENOENT;
		type = btf_type_by_id(btf, field->type);

		/* Reject bitfield member access */
		if (btf_type_kflag(type))
			return -EINVAL;

		/* add offset for anonymous struct type */
		bitoffs += anon_offs + field->offset;
		field_name = next;
		if (next) {
			type = btf_type_skip_modifiers(btf, field->type, NULL);
			if (!type)
				return -ENOENT;
		}
	} while (next);
	return bitoffs / BITS_PER_BYTE;
}

/* btf_put(NULL) is acceptable. */
DEFINE_FREE(btf_put, struct btf *, btf_put(_T))

/* parse typecast: (TYPE[,ASGN])EVENT_FIELD->FIELD[.SUBFIELD...] and set adjust. */
static int wprobe_trigger_typecast_parse(char **field_str_ptr,
					 struct wprobe_trigger_data *wprobe_data)
{
	struct btf *btf __free(btf_put) = NULL;
	const struct btf_type *type;
	char *assign_field;
	char *event_field;
	char *type_field;
	char *type_name;
	int id;
	int adjust;

	type_name = *field_str_ptr + 1;
	event_field = strchr(type_name, ')');
	if (!event_field)
		return -EINVAL;
	*event_field++ = '\0';

	/* Check the optional assign field. */
	assign_field = strchr(type_name, ',');
	if (assign_field)
		*assign_field++ = '\0';

	/* Get the type field name. */
	type_field = strstr(event_field, "->");
	if (!type_field)
		return -EINVAL;
	*type_field = '\0';
	type_field += 2;

	/* find type from BTF */
	id = bpf_find_btf_id(type_name, BTF_KIND_STRUCT, &btf);
	if (id < 0)
		return id;

	type = btf_type_by_id(btf, id);
	if (!type)
		return -EINVAL;

	adjust = get_offset_of_field(btf, type, type_field);
	if (adjust < 0)
		return adjust;
	wprobe_data->adjust = adjust;

	if (assign_field) {
		/* assign_field should be a struct field */
		adjust = get_offset_of_field(btf, type, assign_field);
		if (adjust < 0)
			return adjust;
		wprobe_data->adjust -= adjust;
	}

	*field_str_ptr = event_field;
	return 0;
}
#else
static int wprobe_trigger_typecast_parse(char **field_str_ptr, struct wprobe_trigger_data *wprobe_data)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_PROBE_EVENTS_BTF_ARGS */

static int wprobe_trigger_field_parse(char *field_str, struct trace_event_file *file,
					struct wprobe_trigger_data *wprobe_data)
{
	struct ftrace_event_field *field;
	char *offs;

	if (field_str[0] == '(') {
		int ret = wprobe_trigger_typecast_parse(&field_str, wprobe_data);

		if (ret < 0)
			return ret;
	} else {
		offs = strpbrk(field_str, "+-");
		if (offs) {
			long val;

			if (kstrtol(offs, 0, &val) < 0)
				return -EINVAL;
			wprobe_data->adjust = val;
			*offs = '\0';
		}
	}

	field = trace_find_event_field(file->event_call, field_str);
	if (!field)
		return -ENOENT;
	if (field->size != sizeof(void *))
		return -ENOEXEC;
	wprobe_data->offset = field->offset;
	wprobe_data->field = kstrdup(field_str, GFP_KERNEL);
	if (!wprobe_data->field)
		return -ENOMEM;

	return 0;
}

static int wprobe_trigger_cmd_parse(struct event_command *cmd_ops,
				    struct trace_event_file *file,
				    char *glob, char *cmd,
				    char *param_and_filter)
{
	/*
	 * set_wprobe:EVENT:FIELD[+OFFS]
	 * clear_wprobe:EVENT[:FIELD[+OFFS]]
	 */
	struct wprobe_trigger_data *wprobe_data __free(free_wprobe_trigger_data) = NULL;
	struct event_trigger_data *trigger_data __free(kfree) = NULL;
	struct trace_event_file *wprobe_file;
	struct trace_array *tr = file->tr;
	char *event_str, *field_str;
	bool remove, clear = false;
	struct trace_wprobe *tw;
	char *param, *filter;
	int ret;

	remove = event_trigger_check_remove(glob);

	if (!strcmp(cmd, CLEAR_WPROBE_STR))
		clear = true;

	if (event_trigger_empty_param(param_and_filter))
		return -EINVAL;

	ret = event_trigger_separate_filter(param_and_filter, &param, &filter, true);
	if (ret)
		return ret;

	if (file->event_call->flags & TRACE_EVENT_FL_KPROBE)
		return -EOPNOTSUPP;

	event_str = strsep(&param, ":");

	/* Find target wprobe */
	tw = find_trace_wprobe(event_str, WPROBE_EVENT_SYSTEM);
	if (!tw)
		return -ENOENT;
	/* The target wprobe must not be used (unless clear) */
	if (!remove && !clear && trace_probe_is_enabled(&tw->tp))
		return -EBUSY;

	wprobe_file = find_event_file(tr, WPROBE_EVENT_SYSTEM, event_str);
	if (!wprobe_file)
		return -EINVAL;

	wprobe_data = wprobe_trigger_alloc(tw, wprobe_file, clear);
	if (!wprobe_data)
		return -ENOMEM;

	/* Find target field, which must be equivarent to "void *" */
	field_str = strsep(&param, ":");
	/* trigger removing or clear_wprobe does not need field. */
	if (!remove && !clear && !field_str)
		return -EINVAL;

	if (field_str) {
		ret = wprobe_trigger_field_parse(field_str, file, wprobe_data);
		if (ret < 0)
			return ret;
	}

	trigger_data = trigger_data_alloc(cmd_ops, cmd, param, wprobe_data);
	if (!trigger_data)
		return -ENOMEM;

	trigger_data->private_free = free_private_wprobe_trigger_data;

	/* Up the trigger_data count to make sure nothing frees it on failure */
	event_trigger_init(trigger_data);

	if (remove) {
		event_trigger_unregister(cmd_ops, file, glob+1, trigger_data);
		return 0;
	}

	ret = event_trigger_parse_num(param, trigger_data);
	if (ret)
		return ret;

	ret = event_trigger_set_filter(cmd_ops, file, filter, trigger_data);
	if (ret < 0)
		return ret;

	/* Soft-enable (register) wprobe event on WPROBE_DEFAULT_CLEAR_ADDRESS */
	if (!trace_event_try_get_ref(wprobe_file->event_call)) {
		event_trigger_reset_filter(cmd_ops, trigger_data);
		return -ENODEV;
	}

	if (!clear)
		WRITE_ONCE(tw->addr, WPROBE_DEFAULT_CLEAR_ADDRESS);
	ret = trace_event_enable_disable(wprobe_file, 1, 1);
	if (ret < 0) {
		trace_event_put_ref(wprobe_file->event_call);
		event_trigger_reset_filter(cmd_ops, trigger_data);
		return ret;
	}
	ret = event_trigger_register(cmd_ops, file, glob, trigger_data);
	if (ret) {
		event_trigger_reset_filter(cmd_ops, trigger_data);
		trace_event_enable_disable(wprobe_file, 0, 1);
		trace_event_put_ref(wprobe_file->event_call);
		synchronize_rcu();
		return ret;
	}
	/* Make it NULL to avoid freeing trigger_data and wprobe_data by __free() */
	wprobe_data = NULL;
	event_trigger_free(trigger_data);
	trigger_data = NULL;

	return 0;
}

/* Return event_trigger_data if there is a trigger which points the same wprobe */
static struct event_trigger_data *
wprobe_trigger_find_same(struct event_trigger_data *test,
			 struct trace_event_file *file)
{
	struct wprobe_trigger_data *test_wprobe_data = test->private_data;
	struct wprobe_trigger_data *wprobe_data;
	struct event_trigger_data *iter;

	list_for_each_entry(iter, &file->triggers, list) {
		wprobe_data = iter->private_data;
		if (!wprobe_data ||
		    iter->cmd_ops->trigger_type !=
		    test->cmd_ops->trigger_type)
			continue;
		if (wprobe_data->tw == test_wprobe_data->tw)
			return iter;
	}
	return NULL;
}

static int wprobe_register_trigger(char *glob,
				   struct event_trigger_data *data,
				   struct trace_event_file *file)
{
	int ret = 0;

	lockdep_assert_held(&event_mutex);

	/* The same wprobe is not accept on the same file (event) */
	if (wprobe_trigger_find_same(data, file))
		return -EEXIST;

	if (data->cmd_ops->init) {
		ret = data->cmd_ops->init(data);
		if (ret < 0)
			return ret;
	}

	list_add_rcu(&data->list, &file->triggers);

	update_cond_flag(file);
	ret = trace_event_trigger_enable_disable(file, 1);
	if (ret < 0) {
		list_del_rcu(&data->list);
		update_cond_flag(file);
	}
	return ret;
}

static void wprobe_unregister_trigger(char *glob,
				      struct event_trigger_data *test,
				      struct trace_event_file *file)
{
	struct event_trigger_data *data;

	lockdep_assert_held(&event_mutex);

	data = wprobe_trigger_find_same(test, file);
	if (!data)
		return;

	list_del_rcu(&data->list);
	trace_event_trigger_enable_disable(file, 0);
	update_cond_flag(file);
	if (data->cmd_ops->free)
		data->cmd_ops->free(data);
}

static struct event_command trigger_wprobe_set_cmd = {
	.name			= SET_WPROBE_STR,
	.trigger_type		= ETT_EVENT_WPROBE,
	/* This triggers after when the event is recorded. */
	.flags			= EVENT_CMD_FL_NEEDS_REC,
	.parse			= wprobe_trigger_cmd_parse,
	.reg			= wprobe_register_trigger,
	.unreg			= wprobe_unregister_trigger,
	.set_filter		= set_trigger_filter,
	.trigger		= wprobe_trigger,
	.count_func		= event_trigger_count,
	.print			= wprobe_trigger_print,
	.init			= event_trigger_init,
	.free			= wprobe_trigger_free,
};

static struct event_command trigger_wprobe_clear_cmd = {
	.name			= CLEAR_WPROBE_STR,
	.trigger_type		= ETT_EVENT_WPROBE,
	/* This triggers after when the event is recorded. */
	.flags			= EVENT_CMD_FL_NEEDS_REC,
	.parse			= wprobe_trigger_cmd_parse,
	.reg			= wprobe_register_trigger,
	.unreg			= wprobe_unregister_trigger,
	.set_filter		= set_trigger_filter,
	.trigger		= wprobe_trigger,
	.count_func		= event_trigger_count,
	.print			= wprobe_trigger_print,
	.init			= event_trigger_init,
	.free			= wprobe_trigger_free,
};

static __init int init_trigger_wprobe_cmds(void)
{
	int ret;

	ret = register_event_command(&trigger_wprobe_set_cmd);
	if (WARN_ON(ret < 0))
		return ret;
	ret = register_event_command(&trigger_wprobe_clear_cmd);
	if (WARN_ON(ret < 0))
		unregister_event_command(&trigger_wprobe_set_cmd);

	if (!ret)
		wprobe_trigger_global_enabled = 1;

	return ret;
}
fs_initcall(init_trigger_wprobe_cmds);
#endif /* CONFIG_WPROBE_TRIGGERS */
