// SPDX-License-Identifier: GPL-2.0

#include <net/genetlink.h>
#include "cros_ec_ucsi_nl.h"

static const struct genl_multicast_group nl_mc_grps[] = {
	{ .name = NL_CROS_EC_MC_GRP_NAME },
};

static struct genl_family genl_fam = {
	.name	  = NL_CROS_EC_NAME,
	.version  = NL_CROS_EC_VER,
	.maxattr  = NL_CROS_EC_A_MAX,
	.mcgrps	  = nl_mc_grps,
	.n_mcgrps = ARRAY_SIZE(nl_mc_grps),
};

int nl_cros_ec_register(void)
{
	return genl_register_family(&genl_fam);
}

void nl_cros_ec_unregister(void)
{
	genl_unregister_family(&genl_fam);
}

int nl_cros_ec_bcast_msg(enum nl_cros_ec_msg_dir dir,
			 enum nl_cros_ec_cmd_type cmd_type,
			 u16 offset, const u8 *payload, size_t msg_size)
{
	struct timespec64 ts;
	struct sk_buff *skb;
	int ret = -ENOMEM;
	void *hdr;

	skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	hdr = genlmsg_put(skb, 0, 0, &genl_fam, 0, NL_CROS_EC_C_UCSI);
	if (!hdr)
		goto free_mem;

	ret = nla_put_u8(skb, NL_CROS_EC_A_SRC, NL_CROS_EC_AP);
	if (ret)
		goto cancel;

	ret = nla_put_u8(skb, NL_CROS_EC_A_DIR, dir);
	if (ret)
		goto cancel;

	ret = nla_put_u16(skb, NL_CROS_EC_A_OFFSET, offset);
	if (ret)
		goto cancel;

	ret = nla_put_u8(skb, NL_CROS_EC_A_CMD_TYPE, cmd_type);
	if (ret)
		goto cancel;

	ktime_get_ts64(&ts);
	ret = nla_put_u32(skb, NL_CROS_EC_A_TSTAMP_SEC, (u32)ts.tv_sec);
	if (ret)
		goto cancel;

	ret = nla_put_u32(skb, NL_CROS_EC_A_TSTAMP_USEC,
			  (u32)(ts.tv_nsec/1000));
	if (ret)
		goto cancel;

	ret = nla_put(skb, NL_CROS_EC_A_PAYLOAD, msg_size, payload);
	if (ret)
		goto cancel;

	genlmsg_end(skb, hdr);

	ret = genlmsg_multicast(&genl_fam, skb, 0, 0, GFP_KERNEL);
	if (ret && ret != -ESRCH)
		goto free_mem;

	return 0;
cancel:
	genlmsg_cancel(skb, hdr);
free_mem:
	nlmsg_free(skb);
	return ret;
}
