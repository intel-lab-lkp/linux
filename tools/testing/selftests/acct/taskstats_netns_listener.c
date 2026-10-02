// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/taskstats.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "kselftest.h"

#define TEST_UID 65533
#define TEST_GID 65533
#define ATTACKER_UID 65534
#define ATTACKER_GID 65534
#define VICTIM_COMM "ts-netns-victim"
#define VICTIM_EXIT_CODE 37
#define RECV_TIMEOUT_MS 3000
#define PIPE_TIMEOUT_MS 5000
#define MAX_MSG 16384

struct wire_result {
	int rc;
	int err;
	int query_rc;
	int register_rc;
	uint32_t portid;
	uint32_t uid;
	uint32_t gid;
	unsigned long long netns_ino;
	unsigned long long cap_eff;
	bool found;
	uint32_t id;
	uint32_t ac_pid;
	uint32_t ac_tgid;
	uint32_t ac_uid;
	uint32_t ac_gid;
	uint32_t exit_code;
	char comm[TS_COMM_LEN + 1];
};

static uint32_t nl_seq;

static long long now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return -1;
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_fd(int fd, short events, int timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = events };
	int ret;

	do {
		ret = poll(&pfd, 1, timeout_ms);
	} while (ret < 0 && errno == EINTR);
	if (!ret)
		return -ETIMEDOUT;
	if (ret < 0)
		return -errno;
	if (pfd.revents & events)
		return 0;
	if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
		return -EIO;
	return -EIO;
}

static int read_full_timeout(int fd, void *buf, size_t len, int timeout_ms)
{
	char *pos = buf;
	long long deadline = now_ms() + timeout_ms;

	while (len) {
		long long left = deadline - now_ms();
		ssize_t ret;
		int err;

		if (left <= 0)
			return -ETIMEDOUT;
		err = wait_fd(fd, POLLIN, left > INT32_MAX ? INT32_MAX : (int)left);
		if (err)
			return err;
		ret = read(fd, pos, len);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (!ret)
			return -EPIPE;
		pos += ret;
		len -= ret;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
	const char *pos = buf;

	while (len) {
		ssize_t ret = write(fd, pos, len);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		pos += ret;
		len -= ret;
	}
	return 0;
}

static bool nla_valid(const struct nlattr *attr, size_t remaining)
{
	return remaining >= sizeof(*attr) && attr->nla_len >= NLA_HDRLEN &&
	       attr->nla_len <= remaining;
}

static void *nla_payload(const struct nlattr *attr)
{
	return (char *)attr + NLA_HDRLEN;
}

static size_t nla_payload_len(const struct nlattr *attr)
{
	return attr->nla_len - NLA_HDRLEN;
}

static int open_netlink(uint32_t requested, uint32_t *portid)
{
	struct sockaddr_nl addr = { .nl_family = AF_NETLINK, .nl_pid = requested };
	socklen_t len = sizeof(addr);
	int fd;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	if (fd < 0)
		return -errno;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) ||
	    getsockname(fd, (struct sockaddr *)&addr, &len)) {
		int err = -errno;

		close(fd);
		return err;
	}
	if (len != sizeof(addr) || addr.nl_family != AF_NETLINK) {
		close(fd);
		return -EPROTO;
	}
	*portid = addr.nl_pid;
	return fd;
}

static int add_attr(char *buf, size_t size, size_t *len, uint16_t type,
		    const void *data, size_t data_len)
{
	struct nlattr *attr;
	size_t attr_len = NLA_HDRLEN + data_len;
	size_t aligned = NLA_ALIGN(attr_len);

	if (*len > size || aligned > size - *len)
		return -EMSGSIZE;
	attr = (struct nlattr *)(buf + *len);
	attr->nla_type = type;
	attr->nla_len = attr_len;
	memcpy(nla_payload(attr), data, data_len);
	memset((char *)attr + attr_len, 0, aligned - attr_len);
	*len += aligned;
	return 0;
}

static int send_cmd(int fd, uint32_t portid, uint16_t family, uint8_t cmd,
		    uint16_t attr_type, const void *data, size_t data_len,
		    uint32_t *seq)
{
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	char buf[512] = {};
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct genlmsghdr *genl = (struct genlmsghdr *)(buf + NLMSG_HDRLEN);
	size_t len = NLMSG_HDRLEN + GENL_HDRLEN;
	ssize_t sent;
	int ret;

	nlh->nlmsg_type = family;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nlh->nlmsg_seq = ++nl_seq;
	nlh->nlmsg_pid = portid;
	genl->cmd = cmd;
	genl->version = TASKSTATS_GENL_VERSION;
	ret = add_attr(buf, sizeof(buf), &len, attr_type, data, data_len);
	if (ret)
		return ret;
	nlh->nlmsg_len = len;
	sent = sendto(fd, buf, len, 0, (struct sockaddr *)&kernel,
		      sizeof(kernel));
	if (sent < 0)
		return -errno;
	if ((size_t)sent != len)
		return -EIO;
	*seq = nlh->nlmsg_seq;
	return 0;
}

static ssize_t recv_msg(int fd, void *buf, size_t len, int timeout_ms)
{
	struct sockaddr_nl sender = {};
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	struct msghdr msg = {
		.msg_name = &sender,
		.msg_namelen = sizeof(sender),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};
	ssize_t ret;
	int err;

	err = wait_fd(fd, POLLIN, timeout_ms);
	if (err)
		return err;
	ret = recvmsg(fd, &msg, 0);
	if (ret < 0)
		return -errno;
	if (msg.msg_flags & MSG_TRUNC)
		return -EMSGSIZE;
	if (sender.nl_family != AF_NETLINK || sender.nl_pid)
		return -EPROTO;
	return ret;
}

static int transact(int fd, uint32_t portid, uint16_t family,
		    uint16_t attr_type, const void *data, size_t data_len)
{
	char buf[MAX_MSG];
	uint32_t seq;
	long long deadline;
	int ret;

	ret = send_cmd(fd, portid, family, TASKSTATS_CMD_GET, attr_type,
		       data, data_len, &seq);
	if (ret)
		return ret;
	deadline = now_ms() + RECV_TIMEOUT_MS;
	for (;;) {
		struct nlmsghdr *nlh;
		long long left = deadline - now_ms();
		ssize_t received;
		int remaining;

		if (left <= 0)
			return -ETIMEDOUT;
		received = recv_msg(fd, buf, sizeof(buf), (int)left);
		if (received < 0)
			return received;
		remaining = received;
		for (nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, remaining);
		     nlh = NLMSG_NEXT(nlh, remaining)) {
			struct nlmsgerr *err;

			if (nlh->nlmsg_seq != seq)
				continue;
			if (nlh->nlmsg_type != NLMSG_ERROR)
				continue;
			if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(*err)))
				return -EBADMSG;
			err = NLMSG_DATA(nlh);
			return err->error;
		}
	}
}

static int resolve_family(int fd, uint32_t portid, uint16_t *family)
{
	const char name[] = TASKSTATS_GENL_NAME;
	char buf[MAX_MSG];
	uint32_t seq;
	ssize_t received;
	struct nlmsghdr *nlh;
	int remaining;
	int ret;

	ret = send_cmd(fd, portid, GENL_ID_CTRL, CTRL_CMD_GETFAMILY,
		       CTRL_ATTR_FAMILY_NAME, name, sizeof(name), &seq);
	if (ret)
		return ret;
	received = recv_msg(fd, buf, sizeof(buf), RECV_TIMEOUT_MS);
	if (received < 0)
		return received;
	remaining = received;
	for (nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, remaining);
	     nlh = NLMSG_NEXT(nlh, remaining)) {
		struct genlmsghdr *genl;
		struct nlattr *attr;
		size_t attrs;

		if (nlh->nlmsg_seq != seq)
			continue;
		if (nlh->nlmsg_type == NLMSG_ERROR) {
			struct nlmsgerr *err;

			if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(*err)))
				return -EBADMSG;
			err = NLMSG_DATA(nlh);
			return err->error ? err->error : -ENOENT;
		}
		if (nlh->nlmsg_type != GENL_ID_CTRL ||
		    nlh->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN)
			return -EBADMSG;
		genl = NLMSG_DATA(nlh);
		if (genl->cmd != CTRL_CMD_NEWFAMILY)
			return -EBADMSG;
		attrs = nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
		attr = (struct nlattr *)((char *)genl + GENL_HDRLEN);
		while (nla_valid(attr, attrs)) {
			size_t step = NLA_ALIGN(attr->nla_len);

			if ((attr->nla_type & NLA_TYPE_MASK) == CTRL_ATTR_FAMILY_ID &&
			    nla_payload_len(attr) == sizeof(*family)) {
				memcpy(family, nla_payload(attr), sizeof(*family));
				return 0;
			}
			if (step > attrs)
				return -EBADMSG;
			attrs -= step;
			attr = (struct nlattr *)((char *)attr + step);
		}
		if (attrs)
			return -EBADMSG;
	}
	return -ENOENT;
}

static int listener_cmd(int fd, uint32_t portid, uint16_t family, int cpu,
			bool add)
{
	char mask[32];
	int len;

	len = snprintf(mask, sizeof(mask), "%d", cpu);
	if (len < 0 || len >= (int)sizeof(mask))
		return -EOVERFLOW;
	return transact(fd, portid, family,
			add ? TASKSTATS_CMD_ATTR_REGISTER_CPUMASK :
			      TASKSTATS_CMD_ATTR_DEREGISTER_CPUMASK,
			mask, len + 1);
}

static int query_pid(int fd, uint32_t portid, uint16_t family, uint32_t pid)
{
	return transact(fd, portid, family, TASKSTATS_CMD_ATTR_PID,
			&pid, sizeof(pid));
}

static int decode_nested(struct nlattr *outer, pid_t victim,
			 struct wire_result *result)
{
	struct nlattr *attr = nla_payload(outer);
	size_t remaining = nla_payload_len(outer);
	struct taskstats stats = {};
	uint32_t id = 0;
	bool have_id = false;
	bool have_stats = false;

	while (nla_valid(attr, remaining)) {
		uint16_t type = attr->nla_type & NLA_TYPE_MASK;
		size_t step = NLA_ALIGN(attr->nla_len);

		if ((type == TASKSTATS_TYPE_PID || type == TASKSTATS_TYPE_TGID) &&
		    nla_payload_len(attr) == sizeof(id)) {
			memcpy(&id, nla_payload(attr), sizeof(id));
			have_id = true;
		} else if (type == TASKSTATS_TYPE_STATS &&
			   nla_payload_len(attr) >= sizeof(stats)) {
			memcpy(&stats, nla_payload(attr), sizeof(stats));
			have_stats = true;
		}
		if (step > remaining)
			return -EBADMSG;
		remaining -= step;
		attr = (struct nlattr *)((char *)attr + step);
	}
	if (remaining)
		return -EBADMSG;
	if (!have_id || !have_stats || stats.ac_pid != (uint32_t)victim ||
	    stats.ac_tgid != (uint32_t)victim || id != (uint32_t)victim ||
	    strncmp(stats.ac_comm, VICTIM_COMM, TS_COMM_LEN))
		return 0;
	result->found = true;
	result->id = id;
	result->ac_pid = stats.ac_pid;
	result->ac_tgid = stats.ac_tgid;
	result->ac_uid = stats.ac_uid;
	result->ac_gid = stats.ac_gid;
	result->exit_code = stats.ac_exitcode >> 8;
	memcpy(result->comm, stats.ac_comm, TS_COMM_LEN);
	result->comm[TS_COMM_LEN] = '\0';
	return 1;
}

static int receive_victim(int fd, uint16_t family, pid_t victim,
			  struct wire_result *result, int timeout_ms)
{
	char buf[MAX_MSG];
	long long deadline = now_ms() + timeout_ms;

	for (;;) {
		struct nlmsghdr *nlh;
		long long left = deadline - now_ms();
		ssize_t received;
		int remaining;

		if (left <= 0)
			return 0;
		received = recv_msg(fd, buf, sizeof(buf), (int)left);
		if (received == -ETIMEDOUT)
			return 0;
		if (received < 0)
			return received;
		remaining = received;
		for (nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, remaining);
		     nlh = NLMSG_NEXT(nlh, remaining)) {
			struct genlmsghdr *genl;
			struct nlattr *attr;
			size_t attrs;

			if (nlh->nlmsg_type != family || nlh->nlmsg_pid ||
			    nlh->nlmsg_seq ||
			    nlh->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN)
				continue;
			genl = NLMSG_DATA(nlh);
			if (genl->cmd != TASKSTATS_CMD_NEW)
				continue;
			attrs = nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
			attr = (struct nlattr *)((char *)genl + GENL_HDRLEN);
			while (nla_valid(attr, attrs)) {
				size_t step = NLA_ALIGN(attr->nla_len);
				uint16_t type = attr->nla_type & NLA_TYPE_MASK;
				int ret;

				if (type == TASKSTATS_TYPE_AGGR_PID ||
				    type == TASKSTATS_TYPE_AGGR_TGID) {
					ret = decode_nested(attr, victim, result);
					if (ret)
						return ret;
				}
				if (step > attrs)
					return -EBADMSG;
				attrs -= step;
				attr = (struct nlattr *)((char *)attr + step);
			}
			if (attrs)
				return -EBADMSG;
		}
		if (remaining)
			return -EBADMSG;
	}
}

static unsigned long long read_cap_eff(void)
{
	FILE *file = fopen("/proc/self/status", "re");
	char line[256];
	unsigned long long value = ULLONG_MAX;

	if (!file)
		return value;
	while (fgets(line, sizeof(line), file)) {
		if (sscanf(line, "CapEff:\t%llx", &value) == 1)
			break;
	}
	fclose(file);
	return value;
}

static unsigned long long netns_inode(void)
{
	struct stat st;

	if (stat("/proc/self/ns/net", &st))
		return 0;
	return st.st_ino;
}

static int drop_privileges(uid_t uid, gid_t gid)
{
	struct __user_cap_header_struct header = {
		.version = _LINUX_CAPABILITY_VERSION_3,
	};
	struct __user_cap_data_struct data[2] = {};

	if (setgroups(0, NULL) || setresgid(gid, gid, gid) ||
	    setresuid(uid, uid, uid) || syscall(SYS_capset, &header, data))
		return -errno;
	return 0;
}

static void fill_identity(struct wire_result *result)
{
	result->uid = getuid();
	result->gid = getgid();
	result->netns_ino = netns_inode();
	result->cap_eff = read_cap_eff();
}

static void listener_child(int result_fd, int go_fd, int cpu,
			   bool child_netns, bool attacker, uint32_t requested,
			   uint32_t query_target, pid_t victim, bool deregister)
{
	struct wire_result result = {};
	uint16_t family;
	char go;
	int fd = -1;
	int ret;

	result.query_rc = INT32_MIN;
	result.register_rc = INT32_MIN;
	if (child_netns && unshare(CLONE_NEWNET)) {
		result.rc = -errno;
		goto out;
	}
	if (attacker) {
		ret = drop_privileges(ATTACKER_UID, ATTACKER_GID);
		if (ret) {
			result.rc = ret;
			goto out;
		}
	}
	fill_identity(&result);
	fd = open_netlink(requested, &result.portid);
	if (fd < 0) {
		result.rc = fd;
		goto out;
	}
	ret = resolve_family(fd, result.portid, &family);
	if (ret) {
		result.rc = ret;
		goto out;
	}
	if (attacker) {
		result.query_rc = query_pid(fd, result.portid, family, query_target);
		result.register_rc = listener_cmd(fd, result.portid, family, cpu,
						  true);
	} else {
		result.register_rc = listener_cmd(fd, result.portid, family, cpu,
						  true);
		if (result.register_rc) {
			result.rc = result.register_rc;
			goto out;
		}
		if (deregister) {
			ret = listener_cmd(fd, result.portid, family, cpu, false);
			if (ret) {
				result.rc = ret;
					goto out;
				}
			}
		}
	result.rc = 1;
	if (write_full(result_fd, &result, sizeof(result)))
		goto done;
	ret = read_full_timeout(go_fd, &go, 1, PIPE_TIMEOUT_MS);
	if (ret) {
		result.rc = ret;
		goto final;
	}
	ret = receive_victim(fd, family, victim, &result, RECV_TIMEOUT_MS);
	result.rc = ret < 0 ? ret : 0;
	if (!attacker && !deregister)
		(void)listener_cmd(fd, result.portid, family, cpu, false);
final:
	(void)write_full(result_fd, &result, sizeof(result));
done:
	if (fd >= 0)
		close(fd);
	close(result_fd);
	close(go_fd);
	_exit(result.rc ? KSFT_FAIL : KSFT_PASS);
out:
	result.err = result.rc < 0 ? -result.rc : 0;
	fill_identity(&result);
	(void)write_full(result_fd, &result, sizeof(result));
	goto done;
}

static pid_t spawn_victim(int cpu, int go_fd)
{
	pid_t pid = fork();

	if (pid)
		return pid;
	if (pid < 0)
		return pid;
	{
		cpu_set_t set;
		char go;

		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set) ||
		    prctl(PR_SET_NAME, VICTIM_COMM) ||
		    drop_privileges(TEST_UID, TEST_GID) ||
		    read_full_timeout(go_fd, &go, 1, PIPE_TIMEOUT_MS))
			_exit(125);
	}
	_exit(VICTIM_EXIT_CODE);
}

static bool valid_record(const struct wire_result *result, pid_t victim)
{
	return result->found && result->id == (uint32_t)victim &&
	       result->ac_pid == (uint32_t)victim &&
	       result->ac_tgid == (uint32_t)victim &&
	       result->ac_uid == TEST_UID && result->ac_gid == TEST_GID &&
	       result->exit_code == VICTIM_EXIT_CODE &&
	       !strcmp(result->comm, VICTIM_COMM);
}

static void log_result(const char *role, const struct wire_result *result)
{
	ksft_print_msg("%s: rc=%d errno=%d uid=%u gid=%u CapEff=%016llx\n",
		       role, result->rc, result->err, result->uid, result->gid,
		       result->cap_eff);
	ksft_print_msg("%s: netns=%llu portid=%u query=%d register=%d found=%d\n",
		       role, result->netns_ino, result->portid, result->query_rc,
		       result->register_rc, result->found);
	ksft_print_msg("%s: id=%u pid=%u tgid=%u uid=%u gid=%u comm=%s exit=%u\n",
		       role, result->id, result->ac_pid, result->ac_tgid,
		       result->ac_uid, result->ac_gid, result->comm,
		       result->exit_code);
}

static int reap(pid_t pid)
{
	int status;

	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -errno;
	}
	return status;
}

struct scenario_result {
	struct wire_result registrar;
	struct wire_result attacker;
	struct wire_result admin;
	struct wire_result deregistered;
	struct wire_result unrelated;
	pid_t victim;
	int victim_status;
};

static int run_scenario(int cpu, uint16_t family,
			struct scenario_result *result)
{
	int reg_res[2], reg_go[2], atk_res[2], atk_go[2];
	int adm_res[2], adm_go[2], der_res[2], der_go[2], victim_go[2];
	pid_t reg_pid = -1, atk_pid = -1, adm_pid = -1, der_pid = -1;
	uint32_t other_port;
	int other_fd = -1;
	char go = 'G';
	int ret;

	if (pipe2(reg_res, O_CLOEXEC) || pipe2(reg_go, O_CLOEXEC) ||
	    pipe2(atk_res, O_CLOEXEC) || pipe2(atk_go, O_CLOEXEC) ||
	    pipe2(adm_res, O_CLOEXEC) || pipe2(adm_go, O_CLOEXEC) ||
	    pipe2(der_res, O_CLOEXEC) || pipe2(der_go, O_CLOEXEC) ||
	    pipe2(victim_go, O_CLOEXEC))
		return -errno;
	result->victim = spawn_victim(cpu, victim_go[0]);
	if (result->victim < 0) {
		ret = -errno;
		goto fail;
	}
	reg_pid = fork();
	if (!reg_pid)
		listener_child(reg_res[1], reg_go[0], cpu, true, false, 0, 0,
			       result->victim, false);
	if (reg_pid < 0) {
		ret = -errno;
		goto fail;
	}
	close(reg_res[1]);
	close(reg_go[0]);
	ret = read_full_timeout(reg_res[0], &result->registrar,
				sizeof(result->registrar), PIPE_TIMEOUT_MS);
	if (ret || result->registrar.rc != 1)
		goto fail;
	atk_pid = fork();
	if (!atk_pid)
		listener_child(atk_res[1], atk_go[0], cpu, false, true,
			       result->registrar.portid, reg_pid, result->victim,
			       false);
	if (atk_pid < 0) {
		ret = -errno;
		goto fail;
	}
	close(atk_res[1]);
	close(atk_go[0]);
	ret = read_full_timeout(atk_res[0], &result->attacker,
				sizeof(result->attacker), PIPE_TIMEOUT_MS);
	if (ret || result->attacker.rc != 1)
		goto fail;

	adm_pid = fork();
	if (!adm_pid)
		listener_child(adm_res[1], adm_go[0], cpu, false, false, 0, 0,
			       result->victim, false);
	if (adm_pid < 0) {
		ret = -errno;
		goto fail;
	}
	close(adm_res[1]);
	close(adm_go[0]);
	ret = read_full_timeout(adm_res[0], &result->admin,
				sizeof(result->admin), PIPE_TIMEOUT_MS);
	if (ret || result->admin.rc != 1)
		goto fail;

	der_pid = fork();
	if (!der_pid)
		listener_child(der_res[1], der_go[0], cpu, false, false, 0, 0,
			       result->victim, true);
	if (der_pid < 0) {
		ret = -errno;
		goto fail;
	}
	close(der_res[1]);
	close(der_go[0]);
	ret = read_full_timeout(der_res[0], &result->deregistered,
				sizeof(result->deregistered), PIPE_TIMEOUT_MS);
	if (ret || result->deregistered.rc != 1)
		goto fail;

	other_fd = open_netlink(0, &other_port);
	if (other_fd < 0) {
		ret = other_fd;
		goto fail;
	}
	fill_identity(&result->unrelated);
	result->unrelated.portid = other_port;
	if (write_full(reg_go[1], &go, 1) ||
	    write_full(atk_go[1], &go, 1) ||
	    write_full(adm_go[1], &go, 1) ||
	    write_full(der_go[1], &go, 1) ||
	    write_full(victim_go[1], &go, 1)) {
		ret = -EPIPE;
		goto fail;
	}
	ret = read_full_timeout(reg_res[0], &result->registrar,
				sizeof(result->registrar), PIPE_TIMEOUT_MS);
	if (ret)
		goto fail;
	ret = read_full_timeout(atk_res[0], &result->attacker,
				sizeof(result->attacker), PIPE_TIMEOUT_MS);
	if (ret)
		goto fail;
	ret = read_full_timeout(adm_res[0], &result->admin,
				sizeof(result->admin), PIPE_TIMEOUT_MS);
	if (ret)
		goto fail;
	ret = read_full_timeout(der_res[0], &result->deregistered,
				sizeof(result->deregistered), PIPE_TIMEOUT_MS);
	if (ret)
		goto fail;
	ret = receive_victim(other_fd, family, result->victim,
			     &result->unrelated, 250);
	if (ret < 0 && ret != -ETIMEDOUT)
		goto fail;
	ret = 0;
	(void)reap(reg_pid);
	(void)reap(atk_pid);
	(void)reap(adm_pid);
	(void)reap(der_pid);
	result->victim_status = reap(result->victim);
	log_result("registrar", &result->registrar);
	log_result("colliding receiver", &result->attacker);
	log_result("init_net listener", &result->admin);
	log_result("deregistered listener", &result->deregistered);
	log_result("unrelated port", &result->unrelated);
	ksft_print_msg("victim: pid=%d tgid=%d uid=%u gid=%u comm=%s exit=%d\n",
		       result->victim, result->victim, TEST_UID, TEST_GID,
		       VICTIM_COMM, VICTIM_EXIT_CODE);
	ksft_print_msg("victim: wait_status=%d cpu=%d\n",
		       result->victim_status, cpu);
	close(other_fd);
	return 0;
fail:
	if (reg_pid > 0)
		kill(reg_pid, SIGKILL);
	if (atk_pid > 0)
		kill(atk_pid, SIGKILL);
	if (adm_pid > 0)
		kill(adm_pid, SIGKILL);
	if (der_pid > 0)
		kill(der_pid, SIGKILL);
	if (result->victim > 0)
		kill(result->victim, SIGKILL);
	if (reg_pid > 0)
		(void)reap(reg_pid);
	if (atk_pid > 0)
		(void)reap(atk_pid);
	if (adm_pid > 0)
		(void)reap(adm_pid);
	if (der_pid > 0)
		(void)reap(der_pid);
	if (result->victim > 0)
		(void)reap(result->victim);
	if (other_fd >= 0)
		close(other_fd);
	log_result("registrar setup", &result->registrar);
	log_result("colliding receiver setup", &result->attacker);
	log_result("init_net listener setup", &result->admin);
	log_result("deregistered listener setup", &result->deregistered);
	return ret ? ret : -EINVAL;
}

static int choose_cpu(void)
{
	cpu_set_t set;
	int cpu;

	if (sched_getaffinity(0, sizeof(set), &set))
		return -errno;
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++)
		if (CPU_ISSET(cpu, &set))
			return cpu;
	return -ENODEV;
}

static int netns_preflight(void)
{
	int pipefd[2];
	pid_t pid;
	int err = 0;
	int ret;

	if (pipe2(pipefd, O_CLOEXEC))
		return -errno;
	pid = fork();
	if (!pid) {
		close(pipefd[0]);
		if (unshare(CLONE_NEWNET))
			err = -errno;
		(void)write_full(pipefd[1], &err, sizeof(err));
		_exit(0);
	}
	close(pipefd[1]);
	if (pid < 0) {
		close(pipefd[0]);
		return -errno;
	}
	ret = read_full_timeout(pipefd[0], &err, sizeof(err), PIPE_TIMEOUT_MS);
	close(pipefd[0]);
	(void)reap(pid);
	return ret ? ret : err;
}

int main(void)
{
	struct scenario_result scenario = {};
	unsigned long long init_netns;
	uint32_t portid;
	uint16_t family;
	int fd;
	int cpu;
	int ret;

	ksft_print_header();
	if (geteuid())
		ksft_exit_skip("root or CAP_NET_ADMIN in the initial user namespace is required\n");
	if (!(read_cap_eff() & (1ULL << CAP_NET_ADMIN)))
		ksft_exit_skip("CAP_NET_ADMIN in the initial user namespace is required\n");
	cpu = choose_cpu();
	if (cpu < 0)
		ksft_exit_skip("no usable CPU: %s\n", strerror(-cpu));
	ret = netns_preflight();
	if (ret)
		ksft_exit_skip("network namespace creation unavailable: %s\n",
			       strerror(-ret));
	fd = open_netlink(0, &portid);
	if (fd < 0)
		ksft_exit_skip("Generic Netlink unavailable: %s\n", strerror(-fd));
	ret = resolve_family(fd, portid, &family);
	if (ret) {
		close(fd);
		ksft_exit_skip("taskstats family unavailable: %s\n", strerror(-ret));
	}
	init_netns = netns_inode();
	ksft_print_msg("environment: pid=%d uid=%d gid=%d CapEff=%016llx\n",
		       getpid(), getuid(), getgid(), read_cap_eff());
	ksft_print_msg("environment: netns=%llu cpu=%d family=%u portid=%u\n",
		       init_netns, cpu, family, portid);
	close(fd);

	ksft_set_plan(4);
	ret = run_scenario(cpu, family, &scenario);
	ksft_test_result(!ret && valid_record(&scenario.admin, scenario.victim),
			 "init_net administrator listener receives exit record\n");
	ksft_test_result(!ret && !scenario.unrelated.found &&
			 scenario.unrelated.portid != scenario.admin.portid &&
			 scenario.unrelated.portid != scenario.attacker.portid,
			 "unregistered different port ID receives no exit record\n");
	ksft_test_result(!ret && !scenario.deregistered.found,
			 "explicitly deregistered listener receives no exit record\n");
	ksft_test_result(!ret &&
			 scenario.registrar.portid == scenario.attacker.portid &&
			 scenario.registrar.netns_ino != init_netns &&
			 scenario.attacker.netns_ino == init_netns &&
			 scenario.attacker.uid == ATTACKER_UID &&
			 scenario.attacker.gid == ATTACKER_GID &&
			 !scenario.attacker.cap_eff &&
			 scenario.attacker.query_rc == -EPERM &&
			 scenario.attacker.register_rc == -EPERM &&
			 WIFEXITED(scenario.victim_status) &&
			 WEXITSTATUS(scenario.victim_status) == VICTIM_EXIT_CODE &&
			 valid_record(&scenario.registrar, scenario.victim) &&
			 !scenario.attacker.found,
			 "exit record follows listener netns despite numeric port-ID collision\n");

	ksft_finished();
	return ksft_get_fail_cnt() ? KSFT_FAIL : KSFT_PASS;
}
