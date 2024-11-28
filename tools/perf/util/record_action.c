// SPDX-License-Identifier: GPL-2.0
/**
 * Read perf event output sample data and execute the specified actions.
 */

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <linux/err.h>

#include "util/debug.h"
#include "util/target.h"
#include "util/parse-action.h"
#include "util/record_action.h"

#include "util/bpf_counter.h"
#include "util/bpf_skel/bpf_record_action.h"
#include "util/bpf_skel/record_action.skel.h"

static struct perf_buffer *pb;
static struct record_action_bpf *skel;

struct expr_builtin_output_priv {
	int offset;
	int size;
};

static int bpf_expr_builtin_new(struct evtact_expr *expr,
				void *data __maybe_unused, int size __maybe_unused)
{
	struct expr_builtin_output_priv *priv;

	priv = malloc(sizeof(*priv));
	if (priv == NULL) {
		pr_err("bpf expr builtin priv malloc failed\n");
		return -ENOMEM;
	}

	expr->priv = priv;
	return 0;
}

static void bpf_expr_builtin_free(struct evtact_expr *expr)
{
	zfree(&expr->priv);
}

static int bpf_expr_builtin_eval(struct evtact_expr *expr,
				 void *in, int in_size, void **out, int *out_size)
{
	struct expr_builtin_output_priv *priv = expr->priv;

	if (in_size < priv->size)
		return -EINVAL;

	*out = (u8 *)in + priv->offset;
	*out_size = priv->size;
	return 0;
}

static struct evtact_expr_ops bpf_expr_builtin_common = {
	.new  = bpf_expr_builtin_new,
	.free = bpf_expr_builtin_free,
	.eval = bpf_expr_builtin_eval,
};

static int bpf_expr_builtin_set_ops(struct evtact_expr *expr, u32 opcode)
{
	if (opcode >= EVTACT_EXPR_BUILTIN_TYPE_MAX) {
		pr_err("bpf expr_builtin opcode invalid: %u\n", opcode);
		return -EINVAL;
	}

	expr->ops = &bpf_expr_builtin_common;
	return 0;
}

static struct evtact_expr_class bpf_expr_builtin = {
	.set_ops = bpf_expr_builtin_set_ops,
};

int bpf_perf_record_init(void)
{
	return parse_action_expr__set_class(EVTACT_EXPR_TYPE_BUILTIN,
					    &bpf_expr_builtin);
}

static int set_expr_builtin_output_format(struct evtact_expr *expr,
					  u32 opcode, int *offset, int *format)
{
	int size = 0;
	struct expr_builtin_output_priv *priv = expr->priv;

	switch (opcode) {
	case EVTACT_EXPR_BUILTIN_TYPE_CPU:
		*format = __OUTPUT_FORMAT_TYPE_CPU;
		size = sizeof(u32);
		break;
	case EVTACT_EXPR_BUILTIN_TYPE_PID:
		*format = __OUTPUT_FORMAT_TYPE_PID;
		size = sizeof(u32);
		break;
	case EVTACT_EXPR_BUILTIN_TYPE_TID:
		*format = __OUTPUT_FORMAT_TYPE_TID;
		size = sizeof(u32);
		break;
	case EVTACT_EXPR_BUILTIN_TYPE_COMM:
		*format = __OUTPUT_FORMAT_TYPE_COMM;
		size = __TASK_COMM_MAX_SIZE;
		break;
	default:
		pr_err("set expr builtin output format unknown opcode: %u\n", opcode);
		return -1;
	}

	priv->offset = *offset;
	priv->size = size;
	*offset += size;
	return 0;
}

struct output_args {
	int *num;
	int *offset;
	int *formats;
};

static int do_set_output_format(struct evtact_expr *expr, void *data)
{
	int ret;
	u32 type, opcode;
	struct output_args *args = data;
	int num = *(args->num);

	evtact_expr_id_decode(expr->id, &type, &opcode);
	if (type == EVTACT_EXPR_TYPE_BUILTIN) {
		if (num >= __OUTPUT_FORMATS_MAX_NUM) {
			pr_err("bpf record action output formats too many\n");
			return -1;
		}

		ret = set_expr_builtin_output_format(expr, opcode, args->offset,
						     args->formats + num);
		if (ret)
			return ret;
		num++;
	}

	*(args->num) = num;
	return 0;
}

static int bpf_set_output_format(int *formats)
{
	int ret;
	int offset = 0;
	int num = 0;
	struct output_args args = {
		.num     = &num,
		.offset  = &offset,
		.formats = formats,
	};

	ret = event_actions__for_each_expr(do_set_output_format, &args, true);
	if (ret)
		return ret;

	if (offset > __OUTPUT_DATA_MAX_SIZE) {
		pr_err("bpf record action output too large\n");
		return -1;
	}

	skel->bss->output_format_num = num;
	return 0;
}


struct eval_args {
	void *data;
	__u32 size;
};

static int do_sample_handler(struct evtact_expr *expr, void *data)
{
	int ret;
	struct eval_args *args = data;

	if (expr != NULL && expr->ops->eval != NULL) {
		ret = expr->ops->eval(expr, args->data, args->size, NULL, NULL);
		if (ret)
			return ret;
	}

	return 0;
}

static void sample_callback(void *ctx __maybe_unused, int cpu __maybe_unused,
			    void *data, __u32 size __maybe_unused)
{
	struct __output_data_payload *sample_data = data;
	struct eval_args args = {
		.data = sample_data->__data,
		.size = sample_data->__size,
	};
	(void)event_actions__for_each_expr(do_sample_handler, &args, false);
}

static void lost_callback(void *ctx __maybe_unused, int cpu, __u64 cnt)
{
	fprintf(stderr, "Lost %llu events on CPU #%d\n", cnt, cpu);
}

static int bpf_record_prepare(const char *subsystem, const char *event_name)
{
	int ret, map_fd;

	skel = record_action_bpf__open();
	if (!skel) {
		pr_err("open record-action BPF skeleton failed\n");
		return -1;
	}

	set_max_rlimit();

	ret = bpf_program__set_type(skel->progs.sample_output, BPF_PROG_TYPE_TRACEPOINT);
	if (ret) {
		pr_err("set type record-action BPF skeleton failed\n");
		goto out;
	}

	ret = record_action_bpf__load(skel);
	if (ret) {
		pr_err("load record-action BPF skeleton failed\n");
		goto out;
	}

	ret = bpf_set_output_format(skel->bss->output_formats);
	if (ret)
		goto out;

	map_fd = bpf_map__fd(skel->maps.__sample_data__);
	if (map_fd < 0) {
		pr_err("map fd record-action BPF skeleton failed\n");
		goto out;
	}

	skel->links.sample_output = bpf_program__attach_tracepoint(
		skel->progs.sample_output, subsystem, event_name);
	if (IS_ERR(skel->links.sample_output)) {
		pr_err("attach record-action BPF skeleton failed\n");
		goto out;
	}

	pb = perf_buffer__new(map_fd, 8, sample_callback,
			      lost_callback, NULL, NULL);
	ret = libbpf_get_error(pb);
	if (ret) {
		pr_err("setup record-action perf_buffer failed: %d\n", ret);
		goto out;
	}

	return 0;

out:
	record_action_bpf__destroy(skel);
	return -1;
}

static inline void bpf_record_start(void)
{
	skel->bss->enabled = 1;
}

static inline void bpf_record_stop(void)
{
	skel->bss->enabled = 0;
}

static volatile int done;
static volatile sig_atomic_t child_finished;
static void sig_handler(int sig)
{
	if (sig == SIGCHLD)
		child_finished = 1;

	done = 1;
}

static bool is_bpf_record_supported(struct evlist *evlist,
				    char **subsystem, char **event_name)
{
	struct evsel *evsel;

	if (evlist == NULL) {
		pr_err("--action option should follow a tracer option\n");
		return false;
	}

	/* only one fixed bpf prog and can only be attached to one event. */
	if (evlist->core.nr_entries > 1) {
		pr_err("too many events for specified action\n");
		return false;
	}

	evsel = evlist__last(evlist);
	if (evsel == NULL) {
		pr_err("evlist for bpf record action is empty\n");
		return false;
	}

	if (evsel->core.attr.type != PERF_TYPE_TRACEPOINT) {
		pr_err("bpf record action only supports specifying for tracepoint tracer\n");
		return false;
	}

	*subsystem = strtok_r(evsel->name, ":", event_name);
	if (*subsystem == NULL || event_name == NULL) {
		pr_err("bpf record action tracepoint name format incorrect\n");
		return false;
	}

	return true;
}

int bpf_perf_record(struct evlist *evlist, int argc, const char **argv)
{
	int ret;
	char *subsystem, *event_name;
	struct target target = {
		.system_wide = true,
	};

	if (!is_bpf_record_supported(evlist, &subsystem, &event_name))
		goto out_free_event_actions;

	ret = bpf_record_prepare(subsystem, event_name);
	if (ret)
		goto out_free_event_actions;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGCHLD, sig_handler);

	if (argc > 0) {
		ret = evlist__prepare_workload(evlist, &target, argv, false, NULL);
		if (ret < 0) {
			pr_err("evlist workload create failed\n");
			goto out_destroy_record_action_bpf;
		}
	}

	bpf_record_start();
	evlist__start_workload(evlist);

	while ((ret = perf_buffer__poll(pb, 1000)) >= 0) {
		if (done == 1)
			break;
	}

	bpf_record_stop();

	if (argc > 0) {
		int exit_status;

		if (!child_finished)
			kill(evlist->workload.pid, SIGTERM);

		wait(&exit_status);
	}

out_destroy_record_action_bpf:
	record_action_bpf__destroy(skel);
out_free_event_actions:
	event_actions__free();
	return 0;
}
