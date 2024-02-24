// SPDX-License-Identifier: GPL-2.0
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <unistd.h>
#include "../../../../../include/linux/kernel.h"
#include "aolib.h"

static const size_t buffer_size_kb = 10000;
static char ftrace_path_fmt[] = "ksft-ftrace-XXXXXX";
static char instance_path_fmt[] = "ksft-XXXXXX";
static char *ftrace_path, *instance_path;
static bool ftrace_mounted;
static uint64_t ns_cookie1, ns_cookie2;
static pthread_t tracer_thread;
static bool tracer_thread_created;
static bool tracing_was_dead;

static const char *trace_event_names[__MAX_TRACE_EVENTS] = {
	/* TCP_HASH_EVENT */
	"tcp_hash_bad_header",
	"tcp_hash_md5_required",
	"tcp_hash_md5_unexpected",
	"tcp_hash_md5_mismatch",
	"tcp_hash_ao_required",
	/* TCP_AO_EVENT */
	"tcp_ao_handshake_failure",
	"tcp_ao_wrong_maclen",
	"tcp_ao_mismatch",
	"tcp_ao_key_not_found",
	"tcp_ao_rnext_request",
	/* TCP_AO_EVENT_SK */
	"tcp_ao_synack_no_key",
	/* TCP_AO_EVENT_SNE */
	"tcp_ao_snd_sne_update",
	"tcp_ao_rcv_sne_update"
};

struct expected_trace_point {
	/* required */
	enum trace_events type;
	int family;
	union tcp_addr src;
	union tcp_addr dst;

	/* optional */
	int src_port;
	int dst_port;
	int L3index;

	int fin;
	int syn;
	int rst;
	int psh;
	int ack;

	int keyid;
	int rnext;
	int maclen;
	int sne;

	size_t matched;
};

static struct expected_trace_point *exp_tps;
static size_t exp_tps_nr;
static size_t exp_tps_size;
static pthread_mutex_t exp_tps_mutex = PTHREAD_MUTEX_INITIALIZER;

int __trace_event_expect(enum trace_events type, int family,
			 union tcp_addr src, union tcp_addr dst,
			 int src_port, int dst_port, int L3index,
			 int fin, int syn, int rst, int psh, int ack,
			 int keyid, int rnext, int maclen, int sne)
{
	struct expected_trace_point new_tp = {
		.type		= type,
		.family		= family,
		.src		= src,
		.dst		= dst,
		.src_port	= src_port,
		.dst_port	= dst_port,
		.L3index	= L3index,
		.fin		= fin,
		.syn		= syn,
		.rst		= rst,
		.psh		= psh,
		.ack		= ack,
		.keyid		= keyid,
		.rnext		= rnext,
		.maclen		= maclen,
		.sne		= sne,
		.matched	= 0,
	};
	int ret = 0;

	if (!kernel_config_has(KCONFIG_FTRACE))
		return 0;

	pthread_mutex_lock(&exp_tps_mutex);
	if (exp_tps_nr == exp_tps_size) {
		struct expected_trace_point *tmp;

		if (exp_tps_size == 0)
			exp_tps_size = 10;
		else
			exp_tps_size = exp_tps_size * 1.6;

		tmp = reallocarray(exp_tps, exp_tps_size, sizeof(exp_tps[0]));
		if (!tmp) {
			ret = -ENOMEM;
			goto out;
		}
		exp_tps = tmp;
	}
	exp_tps[exp_tps_nr] = new_tp;
	exp_tps_nr++;
out:
	pthread_mutex_unlock(&exp_tps_mutex);
	return ret;
}

static size_t how_many_matched(void)
{
	size_t i, ret = 0;

	/* We're from the process destructor - not taking the mutex */
	for (i = 0; i < exp_tps_nr; i++)
		ret += exp_tps[i].matched;
	return ret;
}

static void free_expected_events(void)
{
	/* We're from the process destructor - not taking the mutex */
	exp_tps_size = 0;
	exp_tps = NULL;
	free(exp_tps);
}

struct trace_point {
	int family;
	union tcp_addr src;
	union tcp_addr dst;
	unsigned int src_port;
	unsigned int dst_port;
	int L3index;
	unsigned int fin:1,
		     syn:1,
		     rst:1,
		     psh:1,
		     ack:1;

	unsigned keyid;
	unsigned rnext;
	unsigned maclen;

	unsigned sne;
};

static bool lookup_expected_event(int event_type, struct trace_point *e)
{
	size_t i;

	pthread_mutex_lock(&exp_tps_mutex);
	for (i = 0; i < exp_tps_nr; i++) {
		struct expected_trace_point *p = &exp_tps[i];
		size_t sk_size;

		if (p->type != event_type)
			continue;
		if (p->family != e->family)
			continue;
		if (p->family == AF_INET)
			sk_size = sizeof(p->src.a4);
		else
			sk_size = sizeof(p->src.a6);
		if (memcmp(&p->src, &e->src, sk_size))
			continue;
		if (memcmp(&p->dst, &e->dst, sk_size))
			continue;
		if (p->src_port >= 0 && p->src_port != e->src_port)
			continue;
		if (p->dst_port >= 0 && p->dst_port != e->dst_port)
			continue;
		if (p->L3index >= 0 && p->L3index != e->L3index)
			continue;

		if (p->fin >= 0 && p->fin != e->fin)
			continue;
		if (p->syn >= 0 && p->syn != e->syn)
			continue;
		if (p->rst >= 0 && p->rst != e->rst)
			continue;
		if (p->psh >= 0 && p->psh != e->psh)
			continue;
		if (p->ack >= 0 && p->ack != e->ack)
			continue;

		if (p->keyid >= 0 && p->keyid != e->keyid)
			continue;
		if (p->rnext >= 0 && p->rnext != e->rnext)
			continue;
		if (p->maclen >= 0 && p->maclen != e->maclen)
			continue;
		if (p->sne >= 0 && p->sne != e->sne)
			continue;
		p->matched++;
		pthread_mutex_unlock(&exp_tps_mutex);
		return true;
	}
	pthread_mutex_unlock(&exp_tps_mutex);
	return false;
}

static int mount_ftrace(void)
{
	ftrace_path = mkdtemp(ftrace_path_fmt);
	if (!ftrace_path)
		test_error("Can't create temp dir");

	if (mount("tracefs", ftrace_path, "tracefs", 0, "rw"))
		return -errno;

	ftrace_mounted = true;

	return 0;
}

static void unmount_ftrace(void)
{
	if (!ftrace_path)
		return;

	if (ftrace_mounted && umount(ftrace_path))
		test_print("Failed on cleanup: can't unmount tracefs: %m");

	if (rmdir(ftrace_path))
		test_error("Failed on cleanup: can't remove ftrace dir %s",
			   ftrace_path);
}

struct opts_list_t {
	char *opt_name;
	struct opts_list_t *next;
};

static int adjust_trace_options(const char *ftrace_path)
{
	struct opts_list_t *opts_list = NULL;
	char *fopts, *line = NULL;
	size_t buf_len = 0;
	ssize_t line_len;
	int ret = 0;
	FILE *opts;

	fopts = test_sprintf("%s/%s", ftrace_path, "trace_options");
	if (!fopts)
		return -ENOMEM;

	opts = fopen(fopts, "r+");
	if (opts == NULL) {
		ret = -errno;
		goto out_free;
	}

	while ((line_len = getline(&line, &buf_len, opts)) != -1) {
		struct opts_list_t *tmp;

		if (!strncmp(line, "no", 2))
			continue;

		/* XXX: fix show_tcp_state_name() with "nohash-ptr" */
		if (!strncmp(line, "hash-ptr", 8))
			continue;

		tmp = malloc(sizeof(*tmp));
		if (!tmp) {
			ret = -ENOMEM;
			goto out_free_opts_list;
		}
		tmp->next = opts_list;
		tmp->opt_name = test_sprintf("no%s", line);
		if (!tmp->opt_name) {
			ret = -ENOMEM;
			free(tmp);
			goto out_free_opts_list;
		}
		opts_list = tmp;
	}

	while (opts_list) {
		struct opts_list_t *tmp = opts_list;

		fseek(opts, 0, SEEK_SET);
		fwrite(tmp->opt_name, 1, strlen(tmp->opt_name), opts);

		opts_list = opts_list->next;
		free(tmp->opt_name);
		free(tmp);
	}

out_free_opts_list:
	while (opts_list) {
		struct opts_list_t *tmp = opts_list;

		opts_list = opts_list->next;
		free(tmp->opt_name);
		free(tmp);
	}
	free(line);
	fclose(opts);
out_free:
	free(fopts);
	return ret;
}

static int setup_buffer_size(const char *ftrace_path, size_t sz)
{
	char *fbuf_size = test_sprintf("%s/buffer_size_kb", ftrace_path);
	int ret;

	if (!fbuf_size)
		return -1;

	ret = test_echo(fbuf_size, 0, "%zu", sz);
	free(fbuf_size);
	return ret;
}

static int setup_ftrace_instance(void)
{
	char *tmp;

	tmp = test_sprintf("%s/instances/%s", ftrace_path, instance_path_fmt);
	if (!tmp)
		return -ENOMEM;

	instance_path = mkdtemp(tmp);
	if (!instance_path) {
		free(tmp);
		return -errno;
	}

	adjust_trace_options(instance_path);
	setup_buffer_size(instance_path, buffer_size_kb);

	/* instance_path has tmp and gets freed in remove_ftrace_instance() */
	return 0;
}

static void remove_ftrace_instance(void)
{
	if (!instance_path)
		return;
	if (rmdir(instance_path))
		test_print("Failed on cleanup: can't remove ftrace instance %s",
			   instance_path);
	free(instance_path);
}

struct trace_events_list {
	char *line;
	struct trace_events_list *next;
};
static struct trace_events_list *unexpected_events;

static int check_event_type(const char *line)
{
	size_t i;

	/*
	 * This should have been a set or hashmap, but it's a selftest,
	 * so... KISS.
	 */
	for (i = 0; i < __MAX_TRACE_EVENTS; i++) {
		if (!strncmp(trace_event_names[i], line, strlen(trace_event_names[i])))
			return i;
	}
	return -1;
}

static bool event_has_flags(enum trace_events event)
{
	switch (event) {
	case TCP_HASH_BAD_HEADER:
	case TCP_HASH_MD5_REQUIRED:
	case TCP_HASH_MD5_UNEXPECTED:
	case TCP_HASH_MD5_MISMATCH:
	case TCP_HASH_AO_REQUIRED:
	case TCP_AO_HANDSHAKE_FAILURE:
	case TCP_AO_WRONG_MACLEN:
	case TCP_AO_MISMATCH:
	case TCP_AO_KEY_NOT_FOUND:
	case TCP_AO_RNEXT_REQUEST:
		return true;
	default:
		return false;
	}
}

static int tracer_ip_split(int family, char *src, char **addr, char **port)
{
	char *p;

	if (family == AF_INET) {
		/* fomat is <addr>:port, i.e.: 10.0.254.1:7015 */
		*addr = src;
		p = strchr(src, ':');
		if (p == NULL) {
			test_print("Couldn't parse trace event addr:port %s", src);
			return -EINVAL;
		}
		*p++ = '\0';
		*port = p;
		return 0;
	}
	if (family != AF_INET6)
		return -EAFNOSUPPORT;

	/* format is [<addr>]:port, i.e.: [2001:db8:254::1]:7013 */
	*addr = strchr(src, '[');
	p = strchr(src, ']');

	if (p == NULL || *addr == NULL) {
		test_print("Couldn't parse trace event [addr]:port %s", src);
		return -EINVAL;
	}

	*addr = *addr + 1;	/* '[' */
	*p++ = '\0';		/* ']' */
	if (*p != ':') {
		test_print("Couldn't parse trace event :port %s", p);
		return -EINVAL;
	}
	*p++ = '\0';		/* ':' */
	*port = p;
	return 0;
}

static int tracer_scan_address(int family, char *src,
			       union tcp_addr *dst, unsigned int *port)
{
	char *addr, *port_str;
	int ret;

	ret = tracer_ip_split(family, src, &addr, &port_str);
	if (ret)
		return ret;

	if (inet_pton(family, addr, dst) != 1) {
		test_print("Couldn't parse trace event addr %s", addr);
		return -EINVAL;
	}
	errno = 0;
	*port = (unsigned int)strtoul(port_str, NULL, 10);
	if (errno != 0) {
		test_print("Couldn't parse trace event port %s", port_str);
		return -errno;
	}
	return 0;
}

static int tracer_scan_event(const char *line, enum trace_events event,
			     struct trace_point *out)
{
	char *src = NULL, *dst = NULL, *family = NULL;
	char fin, syn, rst, psh, ack;
	int nr_matched, ret = 0;
	uint64_t netns_cookie;

	switch (event) {
	case TCP_HASH_BAD_HEADER:
	case TCP_HASH_MD5_REQUIRED:
	case TCP_HASH_MD5_UNEXPECTED:
	case TCP_HASH_MD5_MISMATCH:
	case TCP_HASH_AO_REQUIRED: {
		nr_matched = sscanf(line, "%*s net=%" PRIu64 " state=%*s family=%ms src=%ms dest=%ms L3index=%d [%c%c%c%c%c]",
			&netns_cookie, &family,
			&src, &dst, &out->L3index,
			&fin, &syn, &rst, &psh, &ack);
		if (nr_matched != 10)
			test_print("Couldn't parse trace event, matched = %d/10",
				   nr_matched);
		break;
	}
	case TCP_AO_HANDSHAKE_FAILURE:
	case TCP_AO_WRONG_MACLEN:
	case TCP_AO_MISMATCH:
	case TCP_AO_KEY_NOT_FOUND:
	case TCP_AO_RNEXT_REQUEST: {
		nr_matched = sscanf(line, "%*s net=%" PRIu64 " state=%*s family=%ms src=%ms dest=%ms L3index=%d [%c%c%c%c%c] keyid=%u rnext=%u maclen=%u",
			&netns_cookie, &family,
			&src, &dst, &out->L3index,
			&fin, &syn, &rst, &psh, &ack,
			&out->keyid, &out->rnext, &out->maclen);
		if (nr_matched != 13)
			test_print("Couldn't parse trace event, matched = %d/13",
				   nr_matched);
		break;
	}
	case TCP_AO_SYNACK_NO_KEY: {
		nr_matched = sscanf(line, "%*s net=%" PRIu64 " state=%*s family=%ms src=%ms dest=%ms keyid=%u rnext=%u",
			&netns_cookie, &family,
			&src, &dst, &out->keyid, &out->rnext);
		if (nr_matched != 6)
			test_print("Couldn't parse trace event, matched = %d/6",
				   nr_matched);
		break;
	}
	case TCP_AO_SND_SNE_UPDATE:
	case TCP_AO_RCV_SNE_UPDATE: {
		nr_matched = sscanf(line, "%*s net=%" PRIu64 " state=%*s family=%ms src=%ms dest=%ms sne=%u",
			&netns_cookie, &family,
			&src, &dst, &out->sne);
		if (nr_matched != 5)
			test_print("Couldn't parse trace event, matched = %d/5",
				   nr_matched);
		break;
	}
	default:
			return -1;
	}

	if (family) {
		if (!strcmp(family, "AF_INET")) {
			out->family = AF_INET;
		} else if (!strcmp(family, "AF_INET6")) {
			out->family = AF_INET6;
		} else {
			test_print("Couldn't parse trace event family %s", family);
			ret = -EINVAL;
			goto out_free;
		}
	}

	if (event_has_flags(event)) {
		out->fin = (fin == 'F');
		out->syn = (syn == 'S');
		out->rst = (rst == 'R');
		out->psh = (psh == '.');
		out->ack = (ack == 'A');

		if ((fin != 'F' && fin != ' ') ||
		    (syn != 'S' && syn != ' ') ||
		    (rst != 'R' && rst != ' ') ||
		    (psh != 'P' && psh != ' ') ||
		    (ack != '.' && ack != ' ')) {
			test_print("Couldn't parse trace event flags %c%c%c%c%c",
				   fin, syn, rst, psh, ack);
			ret = -EINVAL;
			goto out_free;
		}
	}

	if (src && tracer_scan_address(out->family, src, &out->src, &out->src_port)) {
		ret = -EINVAL;
		goto out_free;
	}

	if (dst && tracer_scan_address(out->family, dst, &out->dst, &out->dst_port)) {
		ret = -EINVAL;
		goto out_free;
	}

	if (netns_cookie != ns_cookie1 && netns_cookie != ns_cookie2) {
		test_print("Net namespace filter for trace event didn't work: %" PRIu64 " != %" PRIu64 " OR %" PRIu64,
			   netns_cookie, ns_cookie1, ns_cookie2);
		ret = -EINVAL;
	}

out_free:
	free(src);
	free(dst);
	free(family);
	return ret;
}

static bool tracer_expected_event(const char *line)
{
	int event_type = check_event_type(line);
	struct trace_point tmp = {};

	if (event_type < 0)
		return false;

	if (tracer_scan_event(line, event_type, &tmp))
		return false;

	return lookup_expected_event(event_type, &tmp);
}

struct tracer_cleanup_t {
	FILE *pipe;
	char **line;
};

static void tracer_cleanup(void *arg)
{
	struct tracer_cleanup_t *t = arg;

	fclose(t->pipe);
	free(*(t->line));
}

static void *tracer_thread_func(void *arg)
{
	FILE *trace_pipe = arg;
	size_t buf_len = 0;
	char *line = NULL;
	ssize_t line_len;
	struct tracer_cleanup_t tmp = {
		.pipe = trace_pipe,
		.line = &line,
	};

	pthread_cleanup_push(tracer_cleanup, (void *)&tmp);

	while ((line_len = getline(&line, &buf_len, trace_pipe)) != -1) {
		struct trace_events_list *t;
		bool expected_event;

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		expected_event = tracer_expected_event(line);
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		if (expected_event)
			continue;

		t = malloc(sizeof(*t));
		if (!t)
			test_error("malloc()");
		t->line = line;
		t->next = unexpected_events;
		unexpected_events = t;
		line = NULL;
		buf_len = 0;
	}

	pthread_cleanup_pop(1);
	return NULL;
}

static void setup_trace_thread(void)
{
	FILE *trace_pipe;
	char *path;

	path = test_sprintf("%s/trace_pipe", instance_path);
	if (!path)
		test_error("Not enough memory");

	trace_pipe = fopen(path, "r");
	if (!trace_pipe)
		test_error("fopen()");

	if (pthread_create(&tracer_thread, NULL,
			   tracer_thread_func, (void *)trace_pipe))
		test_error("Failed pthread_create()");
	free(path);
	tracer_thread_created = true;
}

static void stop_trace_thread(void)
{
	void *res;

	if (!tracer_thread_created)
		return;

	if (pthread_cancel(tracer_thread)) {
		test_fail("Can't stop tracer pthread: %m");
		tracing_was_dead = true;
	}
	if (pthread_join(tracer_thread, &res))
		test_print("Can't join tracer pthread: %m");
	if (res != PTHREAD_CANCELED) {
		test_fail("Tracer thread wasn't canceled");
		tracing_was_dead = true;
	}
}

#define dump_events(fmt, ...)				\
	__test_print(__test_msg, fmt, ##__VA_ARGS__)
static void check_free_events(void)
{
	struct trace_events_list *tmp;
	size_t nr;

	if (!kernel_config_has(KCONFIG_FTRACE)) {
		test_skip("kernel config doesn't have ftrace - no checks");
		return;
	}

	if (!unexpected_events) {
		if (tracing_was_dead)
			return;

		nr = how_many_matched();
		if (nr)
			test_ok("Trace events matched expectations: %zu", nr);
		else
			test_ok("No unexpected trace events during the test run");
		return;
	}

	tmp = unexpected_events;
	for (nr = 0; tmp; nr++)
		tmp = tmp->next;

	errno = 0;
	test_fail("Trace events [%zu] were not expected:", nr);
	while (unexpected_events) {
		tmp = unexpected_events;
		unexpected_events = tmp->next;
		dump_events("\t%s", tmp->line);
		free(tmp->line);
		free(tmp);
	}
}

static void test_unset_tracing(void)
{
	stop_trace_thread();
	remove_ftrace_instance();
	unmount_ftrace();
	check_free_events();
	free_expected_events();
}

static int setup_trace_tcp_event(const char *path, const char *name,
				 const char *filter)
{
	char *enable_path, *filter_path;
	int ret;

	enable_path = test_sprintf("%s/events/tcp/%s/enable", path, name);
	if (!enable_path)
		return -ENOMEM;

	filter_path = test_sprintf("%s/events/tcp/%s/filter", path, name);
	if (!filter_path) {
		ret = -ENOMEM;
		goto out_free;
	}

	ret = test_echo(filter_path, 0, "%s", filter);
	if (!ret)
		ret = test_echo(enable_path, 0, "1");

out_free:
	free(filter_path);
	free(enable_path);
	return ret;
}

static int setup_trace_events(void)
{
	char *filter;
	size_t i;
	int ret;

	filter = test_sprintf("net_cookie == %zu || net_cookie == %zu",
			      ns_cookie1, ns_cookie2);
	if (!filter)
		return -ENOMEM;

	for (i = 0; i < __MAX_TRACE_EVENTS; i++) {
		ret = setup_trace_tcp_event(instance_path, trace_event_names[i],
					    filter);
		if (ret)
			break;
	}

	free(filter);
	return ret;
}

int test_setup_tracing(void)
{
	/*
	 * Just a basic protection - this should be called only once from
	 * lib/kconfig. Not thread safe, which is fine as it's early, before
	 * threads are created.
	 */
	static int already_set = 0;
	int err;

	/* Needs net-namespace cookies for filters */
	if (ns_cookie1 == ns_cookie2)
		return -1;

	if (already_set)
		return -1;
	already_set = 1;

	test_add_destructor(test_unset_tracing);
	err = mount_ftrace();
	if (err)
		return err;

	err = setup_ftrace_instance();
	if (err)
		return err;

	err = setup_trace_events();
	if (err)
		return err;
	setup_trace_thread();

	return 0;
}

static int get_ns_cookie(int nsfd, uint64_t *out)
{
	int old_ns = switch_save_ns(nsfd);
	socklen_t size = sizeof(*out);
	int sk;

	sk = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sk < 0) {
		test_print("socket(): %m");
		return -errno;
	}

	if (getsockopt(sk, SOL_SOCKET, SO_NETNS_COOKIE, out, &size)) {
		test_print("getsockopt(SO_NETNS_COOKIE): %m");
		close(sk);
		return -errno;
	}

	close(sk);
	switch_close_ns(old_ns);
	return 0;
}

void test_init_ftrace(int nsfd1, int nsfd2)
{
	get_ns_cookie(nsfd1, &ns_cookie1);
	get_ns_cookie(nsfd2, &ns_cookie2);
	/* Populate kernel config state */
	kernel_config_has(KCONFIG_FTRACE);
}
