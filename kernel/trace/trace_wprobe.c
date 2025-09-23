// SPDX-License-Identifier: GPL-2.0
/*
 * Hardware-breakpoint-based tracing events
 *
 * Copyright (C) 2023, Masami Hiramatsu <mhiramat@kernel.org>
 */
#define pr_fmt(fmt)	"trace_wprobe: " fmt

#include <linux/atomic.h>
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

#include <asm/ptrace.h>

#include "trace.h"
#include "trace_dynevent.h"
#include "trace_probe.h"
#include "trace_probe_kernel.h"
#include "trace_probe_tmpl.h"

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

	/* TODO: match arguments */
	return true;
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
				 struct perf_sample_data *data,
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

	dsize = __get_data_size(&tw->tp, (void *)tw->addr, NULL);

	entry = trace_event_buffer_reserve(&fbuffer, trace_file,
					   sizeof(*entry) + tw->tp.size + dsize);
	if (!entry)
		return;

	entry->ip = instruction_pointer(regs);
	store_trace_args(&entry[1], &tw->tp, (void *)tw->addr, NULL, sizeof(*entry), dsize);

	fbuffer.regs = regs;
	trace_event_buffer_commit(&fbuffer);
}

static void wprobe_perf_handler(struct perf_event *bp,
			      struct perf_sample_data *data,
			      struct pt_regs *regs)
{
	struct trace_wprobe *tw = bp->overflow_handler_context;
	struct event_file_link *link;

	trace_probe_for_each_link_rcu(link, &tw->tp)
		wprobe_trace_handler(tw, data, regs, link->file);
}

static int __register_trace_wprobe(struct trace_wprobe *tw)
{
	struct perf_event_attr attr;

	if (tw->bp_event)
		return -EINVAL;

	hw_breakpoint_init(&attr);
	attr.bp_addr = tw->addr;
	attr.bp_len = tw->len;
	attr.bp_type = tw->type;

	tw->bp_event = register_wide_hw_breakpoint(&attr, wprobe_perf_handler, tw);
	if (IS_ERR((void * __force)tw->bp_event)) {
		int ret = PTR_ERR((void * __force)tw->bp_event);

		tw->bp_event = NULL;
		return ret;
	}

	return 0;
}

static void __unregister_trace_wprobe(struct trace_wprobe *tw)
{
	if (tw->bp_event) {
		unregister_wide_hw_breakpoint(tw->bp_event);
		tw->bp_event = NULL;
	}
}

static void free_trace_wprobe(struct trace_wprobe *tw)
{
	if (tw) {
		trace_probe_cleanup(&tw->tp);
		kfree(tw->symbol);
		kfree(tw);
	}
}
DEFINE_FREE(free_trace_wprobe, struct trace_wprobe *, if (!IS_ERR_OR_NULL(_T)) free_trace_wprobe(_T));

static struct trace_wprobe *alloc_trace_wprobe(const char *group,
					       const char *event,
					       const char *symbol,
					       unsigned long addr,
					       int len, int type, int nargs)
{
	struct trace_wprobe *tw __free(free_trace_wprobe) = NULL;
	int ret;

	tw = kzalloc(struct_size(tw, tp.args, nargs), GFP_KERNEL);
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

	if (!seq_print_ip_sym(s, field->ip, flags | TRACE_ITER_SYM_OFFSET))
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
	struct trace_wprobe *old_tb;
	int ret;

	guard(mutex)(&event_mutex);

	old_tb = find_trace_wprobe(trace_probe_name(&tw->tp),
				   trace_probe_group_name(&tw->tp));
	if (old_tb)
		return -EBUSY;

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
				/* TODO: rollback */
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
		return enable_trace_wprobe(event, NULL);
	case TRACE_REG_PERF_UNREGISTER:
		return disable_trace_wprobe(event, NULL);
	case TRACE_REG_PERF_OPEN:
	case TRACE_REG_PERF_CLOSE:
	case TRACE_REG_PERF_ADD:
	case TRACE_REG_PERF_DEL:
		return 0;
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
	char *symbol = NULL;
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
			/* Invalid format */
			return -EINVAL;
		}
		event = &argv[0][2];
	}

	trace_probe_log_set_index(1);
	ret = parse_address_spec(argv[1], &addr, &type, &len, &symbol);
	if (ret < 0)
		return ret;

	if (!event)
		event = symbol ? symbol : "wprobe";

	argc -= 2; argv += 2;
	tw = alloc_trace_wprobe(group, event, symbol, addr, len, type, argc);
	if (IS_ERR(tw))
		return PTR_ERR(tw);

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
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

	char type_char;

	if (tw->type == HW_BREAKPOINT_R)
		type_char = 'r';
	else if (tw->type == HW_BREAKPOINT_W)
		type_char = 'w';
	else
		type_char = 'x'; /* Should be rw */

	int len;

	if (tw->len == HW_BREAKPOINT_LEN_1)
		len = 1;
	else if (tw->len == HW_BREAKPOINT_LEN_2)
		len = 2;
	else if (tw->len == HW_BREAKPOINT_LEN_4)
		len = 4;
	else
		len = 8;

	if (tw->symbol)
		seq_printf(m, " %c@%s:%d", type_char, tw->symbol, len);
	else
		seq_printf(m, " %c@0x%lx:%d", type_char, tw->addr, len);

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
	struct trace_event_file *file;
	struct trace_wprobe *tw;

	struct perf_event_attr	attr;
	raw_spinlock_t		lock;	/* lock protects attr */
	struct work_struct	work;// TBD: use work + IPI or use sched/raw_syscall event?
	unsigned int		offset;
	long			adjust;
	const char		*field;
	// size must be unsigned long because it should be an address.
	bool			clear;
};

static int trace_wprobe_update_local(struct trace_wprobe *tw,
				     struct perf_event_attr *attr)
{
	struct perf_event *bp = *this_cpu_ptr(tw->bp_event);

	return modify_wide_hw_breakpoint_local(bp, attr);
}

static void wprobe_smp_update_func(void *data)
{
	struct wprobe_trigger_data *trigger_data = data;
	unsigned long flags;

	raw_spin_lock_irqsave(&trigger_data->lock, flags);
	trace_wprobe_update_local(trigger_data->tw, &trigger_data->attr);
	raw_spin_unlock_irqrestore(&trigger_data->lock, flags);
}

static void wprobe_work_func(struct work_struct *work)
{
	struct wprobe_trigger_data *data = container_of(work, struct wprobe_trigger_data, work);

	on_each_cpu(wprobe_smp_update_func, data, false);
}

static void wprobe_trigger(struct event_trigger_data *data,
			   struct trace_buffer *buffer,  void *rec,
			   struct ring_buffer_event *event)
{
	struct wprobe_trigger_data *wprobe_data = data->private_data;
	struct perf_event_attr *attr = &wprobe_data->attr;
	struct trace_wprobe *tw = wprobe_data->tw;
	unsigned long addr, flags;
	int ret = -EBUSY;

	addr = *(unsigned long *)(rec + wprobe_data->offset);
	addr += wprobe_data->adjust;

	raw_spin_lock_irqsave(&wprobe_data->lock, flags);

	if (!wprobe_data->clear) {
		if (tw->addr != WPROBE_DEFAULT_CLEAR_ADDRESS)
			goto unlock;

		tw->addr = attr->bp_addr = addr;
		ret = trace_wprobe_update_local(tw, attr);
		if (WARN_ON_ONCE(ret))
			goto unlock;
		clear_bit(EVENT_FILE_FL_SOFT_DISABLED_BIT, &wprobe_data->file->flags);
	} else {
		if (tw->addr == WPROBE_DEFAULT_CLEAR_ADDRESS)
			goto unlock;
		if (wprobe_data->field && tw->addr != addr)
			goto unlock;

		tw->addr = attr->bp_addr = WPROBE_DEFAULT_CLEAR_ADDRESS;
		ret = trace_wprobe_update_local(tw, attr);
		if (WARN_ON_ONCE(ret))
			goto unlock;
		set_bit(EVENT_FILE_FL_SOFT_DISABLED_BIT, &wprobe_data->file->flags);
	}
	schedule_work(&wprobe_data->work);
unlock:
	raw_spin_unlock_irqrestore(&wprobe_data->lock, flags);
}

static void free_wprobe_trigger_data(struct wprobe_trigger_data *wprobe_data)
{
	if (wprobe_data)
		kfree(wprobe_data->field);
	kfree(wprobe_data);
}

DEFINE_FREE(free_wprobe_trigger_data, struct wprobe_trigger_data *, free_wprobe_trigger_data(_T));

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
	struct perf_event_attr *attr;

	wprobe_data = kzalloc(sizeof(*wprobe_data), GFP_KERNEL);
	if (!wprobe_data)
		return NULL;

	wprobe_data->tw = tw;
	wprobe_data->clear = clear;
	wprobe_data->file = file;

	attr = &wprobe_data->attr;
	hw_breakpoint_init(attr);
	attr->bp_type = tw->type;
	attr->bp_addr = WPROBE_DEFAULT_CLEAR_ADDRESS;
	attr->bp_len = tw->len;

	raw_spin_lock_init(&wprobe_data->lock);
	INIT_WORK(&wprobe_data->work, wprobe_work_func);

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
		free_wprobe_trigger_data(wprobe_data);
	}
}

static const struct event_trigger_ops set_wprobe_trigger_ops = {
	.trigger		= wprobe_trigger,
	.print			= wprobe_trigger_print,
	.init			= event_trigger_init,
	.free			= wprobe_trigger_free,
};

static const struct event_trigger_ops clear_wprobe_trigger_ops = {
	.trigger		= wprobe_trigger,
	.print			= wprobe_trigger_print,
	.init			= event_trigger_init,
	.free			= wprobe_trigger_free,
};

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
	struct ftrace_event_field *field = NULL;
	struct trace_event_file *wprobe_file;
	struct trace_array *tr = file->tr;
	struct trace_event_call *event;
	struct perf_event_attr *attr;
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
	attr = &wprobe_data->attr;

	/* Find target field, which must be equivarent to "void *" */
	field_str = strsep(&param, ":");
	/* trigger removing or clear_wprobe does not need field. */
	if (!remove && !clear && !field_str)
		return -EINVAL;

	if (field_str) {
		char *offs;

		offs = strpbrk(field_str, "+-");
		if (offs) {
			long val;

			if (kstrtol(offs, 0, &val) < 0)
				return -EINVAL;
			wprobe_data->adjust = val;
			*offs = '\0';
		}

		event = file->event_call;
		field = trace_find_event_field(event, field_str);
		if (!field)
			return -ENOENT;

		if (field->size != sizeof(void *))
			return -ENOEXEC;
		wprobe_data->offset = field->offset;
		wprobe_data->field = kstrdup(field_str, GFP_KERNEL);
		if (!wprobe_data->field)
			return -ENOMEM;
	}

	trigger_data = trigger_data_alloc(cmd_ops, cmd, param, wprobe_data);
	if (!trigger_data)
		return -ENOMEM;

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
	tw->addr = attr->bp_addr = WPROBE_DEFAULT_CLEAR_ADDRESS;
	ret = trace_event_enable_disable(wprobe_file, 1, 1);
	if (ret < 0) {
		event_trigger_reset_filter(cmd_ops, trigger_data);
		return ret;
	}
	ret = event_trigger_register(cmd_ops, file, glob, trigger_data);
	if (ret) {
		event_trigger_reset_filter(cmd_ops, trigger_data);
		trace_event_enable_disable(wprobe_file, 0, 1);
		return ret;
	}
	/* Make it NULL to avoid freeing trigger_data and wprobe_data by __free() */
	trigger_data = NULL;
	wprobe_data = NULL;

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

	if (data->ops->init) {
		ret = data->ops->init(data);
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
	if (data->ops->free)
		data->ops->free(data);
}

static const struct event_trigger_ops *
wprobe_get_trigger_ops(char *cmd, char *param)
{
	if (!strcmp(cmd, SET_WPROBE_STR))
		return &set_wprobe_trigger_ops;

	return &clear_wprobe_trigger_ops;
}

static struct event_command trigger_wprobe_set_cmd = {
	.name			= SET_WPROBE_STR,
	.trigger_type		= ETT_EVENT_WPROBE,
	/* This triggers after when the event is recorded. */
	.flags			= EVENT_CMD_FL_NEEDS_REC,
	.parse			= wprobe_trigger_cmd_parse,
	.reg			= wprobe_register_trigger,
	.unreg			= wprobe_unregister_trigger,
	.get_trigger_ops	= wprobe_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

static struct event_command trigger_wprobe_clear_cmd = {
	.name			= CLEAR_WPROBE_STR,
	.trigger_type		= ETT_EVENT_WPROBE,
	/* This triggers after when the event is recorded. */
	.flags			= EVENT_CMD_FL_NEEDS_REC,
	.parse			= wprobe_trigger_cmd_parse,
	.reg			= wprobe_register_trigger,
	.unreg			= wprobe_unregister_trigger,
	.get_trigger_ops	= wprobe_get_trigger_ops,
	.set_filter		= set_trigger_filter,
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
