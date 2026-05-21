// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_alg.h>
#include <linux/bpf.h>
#include <linux/tls.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef SOL_TLS
#define SOL_TLS 282
#endif

#define EXPECTED_BYTES 17312

static void fill_seq(unsigned char *p, size_t n, unsigned char seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		p[i] = (unsigned char)(seed + i);
}

static void bump_memlock(void)
{
	struct rlimit r = { RLIM_INFINITY, RLIM_INFINITY };

	setrlimit(RLIMIT_MEMLOCK, &r);
}

static int run_cmd(const char *cmd)
{
	int ret = system(cmd);

	if (ret == -1)
		return -1;
	if (WIFEXITED(ret))
		return WEXITSTATUS(ret);
	return 1;
}

static int instantiate_aead(const char *name)
{
	struct sockaddr_alg sa = {
		.salg_family = AF_ALG,
	};
	int fd, ret;

	strncpy((char *)sa.salg_type, "aead", sizeof(sa.salg_type) - 1);
	strncpy((char *)sa.salg_name, name, sizeof(sa.salg_name) - 1);

	fd = socket(AF_ALG, SOCK_SEQPACKET, 0);
	if (fd < 0)
		return -errno;

	ret = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (ret < 0)
		ret = -errno;

	close(fd);
	return ret;
}

static bool have_async_pcrypt(void)
{
	FILE *f = fopen("/proc/crypto", "r");
	char line[256];
	bool in_driver = false;
	bool async = false;

	if (!f)
		return false;

	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "driver", 6)) {
			in_driver = strstr(line, "pcrypt(") &&
				    strstr(line, "gcm");
			async = false;
			continue;
		}
		if (in_driver && !strncmp(line, "async", 5)) {
			async = strstr(line, "yes");
			if (async) {
				fclose(f);
				return true;
			}
		}
	}

	fclose(f);
	return false;
}

static bool module_loaded(const char *name)
{
	FILE *f = fopen("/proc/modules", "r");
	char line[256], mod[128];

	if (!f)
		return false;

	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%127s", mod) == 1 && !strcmp(mod, name)) {
			fclose(f);
			return true;
		}
	}

	fclose(f);
	return false;
}

static int make_listener(unsigned short *port)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
		.sin_port = 0,
	};
	socklen_t len = sizeof(addr);
	int fd, one = 1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -errno;

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
		goto err;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		goto err;
	if (listen(fd, 1) < 0)
		goto err;
	if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0)
		goto err;

	*port = ntohs(addr.sin_port);
	return fd;

err:
	close(fd);
	return -errno;
}

static int connect_client(unsigned short port)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
		.sin_port = htons(port),
	};
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -errno;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -errno;
	}

	return fd;
}

static int make_tcp_pair(int *client_fd, int *peer_fd)
{
	unsigned short port = 0;
	int listen_fd, c, p;

	listen_fd = make_listener(&port);
	if (listen_fd < 0)
		return listen_fd;

	c = connect_client(port);
	if (c < 0) {
		close(listen_fd);
		return c;
	}

	p = accept(listen_fd, NULL, NULL);
	close(listen_fd);
	if (p < 0) {
		close(c);
		return -errno;
	}

	*client_fd = c;
	*peer_fd = p;
	return 0;
}

static int enable_ktls_tx(int fd)
{
	struct tls12_crypto_info_aes_gcm_128 info;
	static const char ulp[] = "tls";

	if (setsockopt(fd, IPPROTO_TCP, TCP_ULP, ulp, sizeof(ulp)) < 0)
		return -errno;

	memset(&info, 0, sizeof(info));
	info.info.version = TLS_1_2_VERSION;
	info.info.cipher_type = TLS_CIPHER_AES_GCM_128;
	fill_seq(info.iv, sizeof(info.iv), 0x11);
	fill_seq(info.key, sizeof(info.key), 0x22);
	fill_seq(info.salt, sizeof(info.salt), 0x33);
	fill_seq(info.rec_seq, sizeof(info.rec_seq), 0x44);

	if (setsockopt(fd, SOL_TLS, TLS_TX, &info, sizeof(info)) < 0)
		return -errno;

	return 0;
}

static int attach_bpf(const char *obj_path, int sock_fd, struct bpf_object **obj_out)
{
	struct bpf_object *obj;
	struct bpf_program *prog;
	int map_fd, prog_fd;
	__u32 key = 0;

	obj = bpf_object__open_file(obj_path, NULL);
	if (libbpf_get_error(obj))
		return -EINVAL;
	if (bpf_object__load(obj))
		return -EINVAL;

	prog = bpf_object__find_program_by_name(obj, "apply_bytes_verdict");
	if (!prog)
		return -ENOENT;
	prog_fd = bpf_program__fd(prog);
	if (prog_fd < 0)
		return prog_fd;

	map_fd = bpf_object__find_map_fd_by_name(obj, "sock_map");
	if (map_fd < 0)
		return -ENOENT;

	if (bpf_prog_attach(prog_fd, map_fd, BPF_SK_MSG_VERDICT, 0))
		return -errno;
	if (bpf_map_update_elem(map_fd, &key, &sock_fd, BPF_ANY))
		return -errno;

	*obj_out = obj;
	return 0;
}

static void server_loop(int fd, int out_fd)
{
	unsigned char buf[16384];
	int total = 0;

	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			_exit(1);
		}
		if (!n)
			break;
		total += (int)n;
	}

	(void)write(out_fd, &total, sizeof(total));
	close(fd);
	close(out_fd);
	_exit(0);
}

static int run_case(const char *obj_path, int *server_read)
{
	struct bpf_object *obj = NULL;
	unsigned char buf[4096];
	int client_fd = -1, peer_fd = -1, pipefd[2] = { -1, -1 };
	pid_t pid;
	int status, ret, i;
	ssize_t n, got;

	ret = make_tcp_pair(&client_fd, &peer_fd);
	if (ret)
		return ret;
	if (pipe(pipefd)) {
		close(client_fd);
		close(peer_fd);
		return -errno;
	}

	pid = fork();
	if (pid < 0) {
		close(client_fd);
		close(peer_fd);
		close(pipefd[0]);
		close(pipefd[1]);
		return -errno;
	}
	if (!pid) {
		close(pipefd[0]);
		server_loop(peer_fd, pipefd[1]);
	}

	close(peer_fd);
	close(pipefd[1]);

	ret = attach_bpf(obj_path, client_fd, &obj);
	if (ret)
		goto out;
	ret = enable_ktls_tx(client_fd);
	if (ret)
		goto out;

	fill_seq(buf, sizeof(buf), 0x80);
	for (i = 0; i < 4; i++) {
		n = send(client_fd, buf, sizeof(buf), 0);
		if (n != sizeof(buf)) {
			ret = n < 0 ? -errno : -EIO;
			goto out;
		}
	}

	shutdown(client_fd, SHUT_WR);
	got = read(pipefd[0], server_read, sizeof(*server_read));
	if (got != sizeof(*server_read))
		ret = -EIO;

out:
	close(client_fd);
	close(pipefd[0]);
	if (obj)
		bpf_object__close(obj);
	if (waitpid(pid, &status, 0) < 0)
		return -errno;
	if (!ret && (!WIFEXITED(status) || WEXITSTATUS(status)))
		ret = -EIO;
	return ret;
}

int main(int argc, char **argv)
{
	const char *obj_path = argc > 1 ? argv[1] : "./ktls_async_split.bpf.o";
	int sync_read = 0, async_read = 0, ret;

	ksft_print_header();
	ksft_set_plan(2);
	bump_memlock();

	if (run_cmd("modprobe tls >/dev/null 2>&1") && !module_loaded("tls"))
		ksft_exit_skip("missing tls module\n");

	/* Keep the first run on the synchronous provider. */
	run_cmd("modprobe -r pcrypt >/dev/null 2>&1");
	ret = run_case(obj_path, &sync_read);
	if (ret)
		ksft_exit_fail_msg("sync case failed: %s\n", strerror(-ret));
	if (sync_read != EXPECTED_BYTES)
		ksft_exit_fail_msg("sync case read %d, expected %d\n",
				   sync_read, EXPECTED_BYTES);
	ksft_test_result_pass("sync provider transmits split record\n");

	run_cmd("modprobe af_alg algif_aead pcrypt >/dev/null 2>&1");
	ret = instantiate_aead("pcrypt(generic-gcm-vaes-avx2)");
	if (ret)
		ret = instantiate_aead("pcrypt(gcm(aes))");
	if (ret || !have_async_pcrypt())
		ksft_exit_skip("missing async pcrypt gcm(aes) provider\n");

	ret = run_case(obj_path, &async_read);
	if (ret)
		ksft_exit_fail_msg("async case failed: %s\n", strerror(-ret));
	if (async_read != EXPECTED_BYTES)
		ksft_exit_fail_msg("async case read %d, expected %d\n",
				   async_read, EXPECTED_BYTES);
	ksft_test_result_pass("async provider transmits split record\n");

	ksft_finished();
}
