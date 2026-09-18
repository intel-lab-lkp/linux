// SPDX-License-Identifier: GPL-2.0
/*
 * Functions to read TDX global metadata.
 *
 * This file doesn't compile on its own as it lacks of inclusion
 * of SEAMCALL wrapper primitive which reads global metadata.
 * Include this file to other C file instead.
 */

static __init int get_tdx_sys_info_td_conf(struct tdx_sys_info_td_conf *sysinfo_td_conf)
{
	int ret = 0;
	u64 val;
	int i, j;

	if (!ret && !(ret = read_sys_metadata_field(0x1900000300000000, &val)))
		sysinfo_td_conf->attributes_fixed0 = val;
	if (!ret && !(ret = read_sys_metadata_field(0x1900000300000001, &val)))
		sysinfo_td_conf->attributes_fixed1 = val;
	if (!ret && !(ret = read_sys_metadata_field(0x1900000300000002, &val)))
		sysinfo_td_conf->xfam_fixed0 = val;
	if (!ret && !(ret = read_sys_metadata_field(0x1900000300000003, &val)))
		sysinfo_td_conf->xfam_fixed1 = val;
	if (!ret && !(ret = read_sys_metadata_field(0x9900000100000004, &val)))
		sysinfo_td_conf->num_cpuid_config = val;
	if (!ret && !(ret = read_sys_metadata_field(0x9900000100000008, &val)))
		sysinfo_td_conf->max_vcpus_per_td = val;
	if (sysinfo_td_conf->num_cpuid_config > ARRAY_SIZE(sysinfo_td_conf->cpuid_config_leaves))
		return -EINVAL;
	for (i = 0; i < sysinfo_td_conf->num_cpuid_config; i++)
		if (!ret && !(ret = read_sys_metadata_field(0x9900000300000400 + i, &val)))
			sysinfo_td_conf->cpuid_config_leaves[i] = val;
	if (sysinfo_td_conf->num_cpuid_config > ARRAY_SIZE(sysinfo_td_conf->cpuid_config_values))
		return -EINVAL;
	for (i = 0; i < sysinfo_td_conf->num_cpuid_config; i++)
		for (j = 0; j < 2; j++)
			if (!ret && !(ret = read_sys_metadata_field(0x9900000300000500 + i * 2 + j, &val)))
				sysinfo_td_conf->cpuid_config_values[i][j] = val;

	return ret;
}

static __init int get_tdx_sys_info(struct tdx_sys_info *sysinfo)
{
	int ret = 0;

	ret = ret ?: get_tdx_sys_info_version(&sysinfo->version);

	pr_info("Module version: " TDX_VERSION_FMT "\n",
		sysinfo->version.major_version,
		sysinfo->version.minor_version,
		sysinfo->version.update_version);

	ret = ret ?: get_tdx_sys_info_features(&sysinfo->features);
	ret = ret ?: get_tdx_sys_info_tdmr(&sysinfo->tdmr);
	ret = ret ?: get_tdx_sys_info_td_ctrl(&sysinfo->td_ctrl);
	ret = ret ?: get_tdx_sys_info_td_conf(&sysinfo->td_conf);

	/*
	 * The kernel supports using TDX without DPAMT, so
	 * avoid reporting failure if it's not supported. Don't
	 * try to support buggy TDX modules that advertise
	 * DPAMT but don't expose the metadata.
	 */
	if (!ret && tdx_supports_dynamic_pamt(sysinfo))
		ret = get_tdx_sys_info_tdmr_dpamt(&sysinfo->tdmr);

	return ret;
}
