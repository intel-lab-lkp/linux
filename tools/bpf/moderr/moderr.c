// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2025 Samsung */
#include <argp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <stdlib.h>
#include <string.h>
#include "moderr.h"
#include "moderr.skel.h"

static struct env {
	bool verbose;
	char modname[MODULE_NAME_LEN];
	enum modfunc func;
	bool trace;
	int errinj;
} env;

const char *argp_program_version = "moderr 0.1";
const char *argp_program_bug_address = "<da.gomez@samsung.com>";
const char argp_program_doc[] =
"BPF moderr application.\n"
"\n"
"It injects errors in module initialization\n"
"\nUSAGE: "
"moderr [-m <module_name>] [-f <function_name>] [-e <errno>]\n";

static volatile bool exiting = false;

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))

static const struct argp_option opts[] = {
	{ "verbose", 'v', NULL, 0, "Verbose debug output" },
	{ "trace", 't', NULL, 0, "Enable trace output", 0 },
	{ "modname", 'm', "MODNAME", 0, "Trace this module name only", 0 },
	{ "modfunc", 'f', "MODFUNC", 0, "Trace this module function only", 0 },
	{ "list", 'l', NULL, 0, "List available module functions", 0 },
	{ "error", 'e', "ERROR", 0, "Inject this error", 0 },
	{ NULL, 'h', NULL, OPTION_HIDDEN, "Show the full help", 0 },
	{},
};

static void help_modfunc(void)
{
	printf("\nAvailable modfunc options are:\n"
	       "- complete_formation\n"
	       "- do_init_module\n"
	       "- module_enable_rodata_ro_after_init\n\n");
}

static enum modfunc string_to_modfunc(char *arg)
{
	if (strncmp(arg, "complete_formation", strlen(arg)) == 0)
		return COMPLETE_FORMATION;

	if (strncmp(arg, "do_init_module", strlen(arg)) == 0)
		return DO_INIT_MODULE;

	if (strncmp(arg, "module_enable_rodata_ro_after_init", strlen(arg)) ==
	    0)
		return MODULE_ENABLE_RODATA_AFTER_INIT;

	return UNKNOWN;
}

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	switch (key) {
	case 'h':
		argp_state_help(state, stderr, ARGP_HELP_STD_HELP);
		break;
	case 'l':
		help_modfunc();
		argp_usage(state);
		break;
	case 'v':
		env.verbose = true;
		break;
	case 'm':
		if (strlen(arg) + 1 > MODULE_NAME_LEN) {
			fprintf(stderr, "module name error\n");
			argp_usage(state);
		}
		strncpy(env.modname, arg, sizeof(env.modname) - 1);
		break;
	case 'f':
		if (strlen(arg) + 1 > MODULE_FUNC_LEN) {
			fprintf(stderr, "module function too long\n");
			argp_usage(state);
		}
		env.func = string_to_modfunc(arg);
		if (!env.func) {
			fprintf(stderr, "invalid module function\n");
			help_modfunc();
			argp_usage(state);
		}
		break;
	case 'e':
		env.errinj = atoi(arg);
		break;
	case 't':
		env.trace = true;
		break;
	case ARGP_KEY_ARG:
		argp_usage(state);
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static const struct argp argp = {
	.options = opts,
	.parser = parse_arg,
	.doc = argp_program_doc,
};

static int libbpf_print_fn(enum libbpf_print_level level, const char *format,
			   va_list args)
{
	if (level == LIBBPF_DEBUG && !env.verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sig_handler(int sig)
{
	exiting = true;
}

static int handle_event(void *ctx, void *data, size_t data_sz)
{
	const struct event *e = data;

	if (!env.trace)
		return 0;

	if (e->dbg) {
		if (env.verbose)
			printf("%s\n", e->msg);
		return 0;
	}

	printf("%-10s %-5d %-20s\n", e->modname, e->err,
	       modfunc_to_string(e->func));

	return 0;
}

int main(int argc, char **argv)
{
	struct ring_buffer *rb = NULL;
	struct moderr_bpf *obj;
	int err;

	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (err)
		return err;

	if (!strlen(env.modname) || !env.func) {
		fprintf(stderr, "missing arguments\n");
		return EXIT_FAILURE;
	}

	libbpf_set_print(libbpf_print_fn);

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	obj = moderr_bpf__open();
	if (!obj) {
		fprintf(stderr, "failed to open and load BPF object\n");
		return 1;
	}

	obj->rodata->filter_modname = true;
	strncpy(obj->rodata->targ_modname, env.modname, MODULE_NAME_LEN - 1);
	obj->rodata->targ_modname[MODULE_NAME_LEN - 1] = '\0';

	obj->rodata->filter_modfunc = true;
	obj->rodata->targ_modfunc = env.func;

	if (env.errinj) {
		obj->rodata->set_errinj = true;
		obj->rodata->targ_errinj = env.errinj;
	}

	err = moderr_bpf__load(obj);
	if (err) {
		fprintf(stderr, "failed to load and verify BPF object\n");
		goto cleanup;
	}

	err = moderr_bpf__attach(obj);
	if (err) {
		fprintf(stderr, "failed to attach BPF object\n");
		goto cleanup;
	}

	printf("Monitoring module error injection... Hit Ctrl-C to end.\n");

	rb = ring_buffer__new(bpf_map__fd(obj->maps.rb), handle_event, NULL,
			      NULL);
	if (!rb) {
		err = -1;
		fprintf(stderr, "failed to create ring buffer\n");
		goto cleanup;
	}

	if (env.trace)
		printf("%-10s %-5s %-20s\n", "MODULE", "ERROR", "FUNCTION");

	while (!exiting) {
		err = ring_buffer__poll(rb, 100);
		if (err == -EINTR) {
			err = 0;
			break;
		}
		if (err < 0) {
			fprintf(stderr, "error polling ring buffer: %d\n", err);
			break;
		}
	}

	printf("\n");

cleanup:
	ring_buffer__free(rb);
	moderr_bpf__destroy(obj);

	return err < 0 ? -err : 0;
}
