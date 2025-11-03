#include <linux/init.h>
#include <linux/sysctl.h>
#include <net/netfilter/nf_tables.h>
#include <net/net_namespace.h>

#ifdef CONFIG_SYSCTL
enum nf_ct_sysctl_index {
	NF_SYSCTL_NFT_JUMPS_MAX,
	NF_SYSCTL_NFT_LAST_SYSCTL
};

static struct ctl_table nf_tables_sysctl_table[] = {
	[NF_SYSCTL_NFT_JUMPS_MAX] = {
		.procname       = "nf_tables_jumps_max_netns",
		.data           = &init_net.nf.nf_tables_jumps_max_netns,
		.maxlen         = sizeof(init_net.nf.nf_tables_jumps_max_netns),
		.mode           = 0644,
		.proc_handler   = proc_dointvec,
		.extra1		= SYSCTL_ONE,
		.extra2		= SYSCTL_INT_MAX,
	},
};

#define NFT_TABLE_DEFAULT_JUMPS_MAX 256000

static int __net_init nf_tables_sysctl_init(struct net *net)
{
	struct ctl_table *table = nf_tables_sysctl_table;

	BUILD_BUG_ON(ARRAY_SIZE(nf_tables_sysctl_table) != NF_SYSCTL_NFT_LAST_SYSCTL);

	if (net_eq(net, &init_net)) {
		net->nf.nf_tables_jumps_max_netns = NFT_TABLE_DEFAULT_JUMPS_MAX;
	} else {
		table = kmemdup(nf_tables_sysctl_table,
				sizeof(nf_tables_sysctl_table), GFP_KERNEL);
		if (!table)
			return -ENOMEM;

		net->nf.nf_tables_jumps_max_netns =
			init_net.nf.nf_tables_jumps_max_netns;
		table[NF_SYSCTL_NFT_JUMPS_MAX].data =
			&net->nf.nf_tables_jumps_max_netns;

		if (net->user_ns != &init_user_ns)
			table[NF_SYSCTL_NFT_JUMPS_MAX].mode &= ~0222;
	}

	net->nf.nf_tables_dir_header =
		register_net_sysctl_sz(net, "net/netfilter", table,
				       ARRAY_SIZE(nf_tables_sysctl_table));
	if (!net->nf.nf_tables_dir_header)
		goto err_tbl_free;

	return 0;

err_tbl_free:
	if (table != nf_tables_sysctl_table)
		kfree(table);

	return -ENOMEM;
}

static void nf_tables_sysctl_exit(struct net *net)
{
	const struct ctl_table *table;

	unregister_net_sysctl_table(net->nf.nf_tables_dir_header);
	table = net->nf.nf_tables_dir_header->ctl_table_arg;
	if (!net_eq(net, &init_net))
		kfree(table);
}

static struct pernet_operations nf_tables_sysctl_net_ops = {
	.init = nf_tables_sysctl_init,
	.exit = nf_tables_sysctl_exit,
};

int __init netfilter_nf_tables_sysctl_init(void)
{
	return register_pernet_subsys(&nf_tables_sysctl_net_ops);
}

void netfilter_nf_tables_sysctl_fini(void)
{
	unregister_pernet_subsys(&nf_tables_sysctl_net_ops);
}
#else
int __init netfilter_nf_tables_sysctl_init(void) { return 0; }
void netfilter_nf_tables_sysctl_fini(void) {}
#endif /* CONFIG_SYSCTL */
