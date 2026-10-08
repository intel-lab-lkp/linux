// SPDX-License-Identifier: GPL-2.0

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/genetlink.h>
#include <linux/netlink.h>

#include "kselftest.h"

#define REQUEST_SIZE 128
#define REPLY_SIZE 4096

struct genl_request {
	struct nlmsghdr nlh;
	struct genlmsghdr genl;
	char attrs[REQUEST_SIZE];
};

static int add_attr(struct genl_request *request, size_t size, uint16_t type,
		    const void *data, size_t data_size)
{
	size_t offset = NLMSG_ALIGN(request->nlh.nlmsg_len);
	size_t attr_size = NLA_HDRLEN + data_size;
	size_t aligned_size = NLA_ALIGN(attr_size);
	struct nlattr *attr;

	if (offset + aligned_size > size)
		return -E2BIG;

	attr = (struct nlattr *)((char *)request + offset);
	attr->nla_len = attr_size;
	attr->nla_type = type;
	memcpy((char *)attr + NLA_HDRLEN, data, data_size);
	memset((char *)attr + attr_size, 0, aligned_size - attr_size);
	request->nlh.nlmsg_len = offset + aligned_size;

	return 0;
}

static int next_attr(const char **cursor, size_t *remaining,
		     struct nlattr **attr)
{
	size_t length, aligned_length;

	if (!*remaining)
		return 0;
	if (*remaining < NLA_HDRLEN)
		return -EBADMSG;

	*attr = (struct nlattr *)*cursor;
	length = (*attr)->nla_len;
	if (length < NLA_HDRLEN || length > *remaining)
		return -EBADMSG;
	aligned_length = NLA_ALIGN(length);
	if (aligned_length > *remaining)
		return -EBADMSG;

	*cursor += aligned_length;
	*remaining -= aligned_length;
	return 1;
}

static int find_op_id(const struct nlattr *op_policy, bool *found)
{
	const char *cursor = (const char *)op_policy + NLA_HDRLEN;
	size_t remaining = op_policy->nla_len - NLA_HDRLEN;
	struct nlattr *attr;
	int ret;

	while ((ret = next_attr(&cursor, &remaining, &attr)) > 0) {
		if ((attr->nla_type & NLA_TYPE_MASK) == CTRL_CMD_GETPOLICY)
			*found = true;
	}

	return ret;
}

static int parse_policy_message(const struct nlmsghdr *nlh, bool *found)
{
	const char *cursor = (const char *)nlh + NLMSG_LENGTH(GENL_HDRLEN);
	size_t remaining = nlh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
	struct nlattr *attr;
	int ret;

	while ((ret = next_attr(&cursor, &remaining, &attr)) > 0) {
		if ((attr->nla_type & NLA_TYPE_MASK) == CTRL_ATTR_OP_POLICY) {
			ret = find_op_id(attr, found);
			if (ret < 0)
				return ret;
		}
	}

	return ret;
}

static int check_policy_dump(int fd)
{
	struct genl_request request = {
		.nlh = {
			.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN),
			.nlmsg_type = GENL_ID_CTRL,
			.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP | NLM_F_ACK,
			.nlmsg_seq = 1,
		},
		.genl = {
			.cmd = CTRL_CMD_GETPOLICY,
			.version = 1,
		},
	};
	char reply[REPLY_SIZE];
	const __u16 family_id = GENL_ID_CTRL;
	const __u32 op = CTRL_CMD_GETPOLICY;
	bool found_op = false;
	struct sockaddr_nl kernel = {
		.nl_family = AF_NETLINK,
	};
	ssize_t sent;
	int ret;

	ret = add_attr(&request, sizeof(request), CTRL_ATTR_FAMILY_ID,
		       &family_id, sizeof(family_id));
	if (ret)
		return ret;
	ret = add_attr(&request, sizeof(request), CTRL_ATTR_OP,
		       &op, sizeof(op));
	if (ret)
		return ret;

	sent = sendto(fd, &request, request.nlh.nlmsg_len, 0,
		      (struct sockaddr *)&kernel, sizeof(kernel));
	if (sent < 0)
		return -errno;
	if (sent != request.nlh.nlmsg_len)
		return -EIO;

	for (;;) {
		ssize_t received = recv(fd, reply, sizeof(reply), 0);
		size_t offset = 0;

		if (received < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}

		while (offset + NLMSG_HDRLEN <= received) {
			struct nlmsghdr *nlh = (struct nlmsghdr *)(reply + offset);
			size_t aligned_length;

			if (nlh->nlmsg_len < NLMSG_HDRLEN ||
			    nlh->nlmsg_len > received - offset)
				return -EBADMSG;
			if (nlh->nlmsg_seq != 1) {
				offset += NLMSG_ALIGN(nlh->nlmsg_len);
				continue;
			}
			if (nlh->nlmsg_type == NLMSG_ERROR) {
				int error;

				if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(error)))
					return -EBADMSG;
				error = *(int *)NLMSG_DATA(nlh);
				if (error)
					return error;
			} else if (nlh->nlmsg_type == NLMSG_DONE) {
				if (nlh->nlmsg_flags & NLM_F_DUMP_INTR)
					return -EINTR;
				return found_op ? 0 : -ENOENT;
			} else if (nlh->nlmsg_type == GENL_ID_CTRL) {
				if (nlh->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN))
					return -EBADMSG;
				ret = parse_policy_message(nlh, &found_op);
				if (ret < 0)
					return ret;
			}

			aligned_length = NLMSG_ALIGN(nlh->nlmsg_len);
			if (aligned_length > received - offset)
				return -EBADMSG;
			offset += aligned_length;
		}
	}
}

int main(void)
{
	struct sockaddr_nl local = {
		.nl_family = AF_NETLINK,
	};
	int fd, ret;

	ksft_print_header();
	ksft_set_plan(1);

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	if (fd < 0)
		ksft_exit_fail_perror("opening generic-netlink socket");
	if (bind(fd, (struct sockaddr *)&local, sizeof(local))) {
		close(fd);
		ksft_exit_fail_perror("binding generic-netlink socket");
	}

	ret = check_policy_dump(fd);
	if (close(fd) && !ret)
		ret = -errno;
	if (ret == -ENOENT) {
		ksft_test_result_fail("missing policy op id %u\n",
				      CTRL_CMD_GETPOLICY);
	} else if (ret) {
		errno = -ret;
		ksft_test_result_fail("GETPOLICY dump failed: %s\n",
				      strerror(errno));
	} else {
		ksft_test_result_pass("policy dump identified command id %u\n",
				      CTRL_CMD_GETPOLICY);
	}
	ksft_finished();
}
