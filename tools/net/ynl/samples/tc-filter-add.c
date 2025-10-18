// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <linux/pkt_sched.h>
#include <linux/tc_act/tc_vlan.h>
#include <linux/tc_act/tc_gact.h>
#include <net/if.h>

#include <ynl.h>

#include "tc-user.h"

int main(int argc, char **argv)
{
	struct tc_newtfilter_req *req;
	struct tc_act_attrs *acts;
	struct tc_vlan p = {
		.v_action = TCA_VLAN_ACT_PUSH
	};
	__u16 flags = NLM_F_EXCL | NLM_F_CREATE;
	struct ynl_error yerr;
	struct ynl_sock *ys;
	int ifi;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <interface_name>\n", argv[0]);
		return 1;
	}
	ifi = if_nametoindex(argv[1]);
	if (!ifi) {
		perror("if_nametoindex");
		return 1;
	}

	ys = ynl_sock_create(&ynl_tc_family, &yerr);
	if (!ys) {
		fprintf(stderr, "YNL: %s\n", yerr.msg);
		return 1;
	}

	req = tc_newtfilter_req_alloc();
	if (!req) {
		fprintf(stderr, "tc_newtfilter_req_alloc failed\n");
		goto err_destroy;
	}
	memset(req, 0, sizeof(*req));

	acts = tc_act_attrs_alloc(2);
	if (!acts) {
		fprintf(stderr, "tc_act_attrs_alloc\n");
		goto err_act;
	}
	memset(acts, 0, sizeof(*acts));

	req->_hdr.tcm_ifindex = ifi;
	req->_hdr.tcm_parent = TC_H_MAKE(TC_H_CLSACT, TC_H_MIN_INGRESS);
	req->_hdr.tcm_info = TC_H_MAKE((2211 << 16), htons(0x8100));
	req->chain = 0;

	tc_newtfilter_req_set_nlflags(req, flags);
	tc_newtfilter_req_set_kind(req, "flower");
	tc_newtfilter_req_set_options_flower_key_vlan_id(req, 255);
	tc_newtfilter_req_set_options_flower_key_vlan_prio(req, 5);
	tc_newtfilter_req_set_options_flower_key_num_of_vlans(req, 3);

	__tc_newtfilter_req_set_options_flower_act(req, acts, 2);

	tc_act_attrs_set_kind(&acts[0], "vlan");
	tc_act_attrs_set_options_vlan_parms(&acts[0], &p, sizeof(p));
	tc_act_attrs_set_options_vlan_push_vlan_id(&acts[0], 255);
	tc_act_attrs_set_kind(&acts[1], "vlan");
	tc_act_attrs_set_options_vlan_parms(&acts[1], &p, sizeof(p));
	tc_act_attrs_set_options_vlan_push_vlan_id(&acts[1], 555);

	tc_newtfilter_req_set_options_flower_flags(req, 0);
	tc_newtfilter_req_set_options_flower_key_eth_type(req, htons(0x8100));

	if (tc_newtfilter(ys, req))
		fprintf(stderr, "YNL: %s\n", ys->err.msg);

	tc_newtfilter_req_free(req);
	ynl_sock_destroy(ys);
	return 0;

err_act:
	tc_newtfilter_req_free(req);
err_destroy:
	ynl_sock_destroy(ys);
	return 2;
}
