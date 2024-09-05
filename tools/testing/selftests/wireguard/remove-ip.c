// SPDX-License-Identifier: GPL-2.0
#include <linux/wireguard.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netlink/socket.h>
#include <netlink/netlink.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>

#define CURVE25519_KEY_SIZE 32

const char *usage = "Usage: remove-ip INTERFACE_NAME PEER_PUBLIC_KEY_HEX IP_VERSION IP";

char h2b(char c)
{
	if ('0' <= c && c <= '9')
		return c - '0';
	else if ('a' <= c && c <= 'f')
		return 10 + (c - 'a');

	return -1;
}

int parse_key(const char *raw, unsigned char key[CURVE25519_KEY_SIZE])
{
	int ret = 0;
	int i;

	for (i = 0; i < CURVE25519_KEY_SIZE; i++) {
		char h, l;

		h = h2b(raw[0]);
		if (h < 0)
			return -1;

		l = h2b(raw[1]);
		if (l < 0)
			return -1;

		key[i] = (h << 4) | l;
		raw += 2;
	}

	return 0;
}

int main(int argc, char **argv)
{
	unsigned char addr[sizeof(struct in6_addr)];
	unsigned char pub_key[CURVE25519_KEY_SIZE];
	struct nl_sock *sock;
	struct nl_msg *msg;
	int addr_len;
	int family;
	int cidr;
	int af;

	if (argc < 5) {
		printf("Not enough arguments.\n\n%s\n", usage);
		return -1;
	}

	if (parse_key(argv[2], pub_key)) {
		printf("Could not parse public key\n");
		return -1;
	}

	switch (argv[3][0]) {
	case '4':
		af = AF_INET;
		addr_len = sizeof(struct in_addr);
		cidr = 32;
		break;
	case '6':
		af = AF_INET6;
		addr_len = sizeof(struct in6_addr);
		cidr = 128;
		break;
	default:
		printf("Invalid IP version\n");
		return -1;
	}

	if (inet_pton(af, argv[4], &addr) <= 0) {
		printf("Could not parse IP address\n");
		return -1;
	}

	sock = nl_socket_alloc();
	genl_connect(sock);
	family = genl_ctrl_resolve(sock, WG_GENL_NAME);
	msg = nlmsg_alloc();
	genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, family, 0, NLM_F_ECHO,
		    WG_CMD_SET_DEVICE, WG_GENL_VERSION);
	nla_put_string(msg, WGDEVICE_A_IFNAME, argv[1]);

	struct nlattr *peers = nla_nest_start(msg, WGDEVICE_A_PEERS);
	struct nlattr *peer0 = nla_nest_start(msg, 0);

	nla_put(msg, WGPEER_A_PUBLIC_KEY, CURVE25519_KEY_SIZE, pub_key);

	struct nlattr *allowed_ips = nla_nest_start(msg, WGPEER_A_ALLOWEDIPS);
	struct nlattr *allowed_ip0 = nla_nest_start(msg, 0);

	nla_put_u16(msg, WGALLOWEDIP_A_FAMILY, af);
	nla_put(msg, WGALLOWEDIP_A_IPADDR, addr_len, &addr);
	nla_put_u8(msg, WGALLOWEDIP_A_CIDR_MASK, cidr);
	nla_put_u32(msg, WGALLOWEDIP_A_FLAGS, WGALLOWEDIP_F_REMOVE_ME);
	nla_nest_end(msg, allowed_ip0);
	nla_nest_end(msg, allowed_ips);
	nla_nest_end(msg, peer0);
	nla_nest_end(msg, peers);

	int err = nl_send_sync(sock, msg);

	if (err < 0) {
		char message[256];

		nl_perror(err, message);
		printf("An error occurred: %d - %s\n", err, message);
	}

	return err;
}
