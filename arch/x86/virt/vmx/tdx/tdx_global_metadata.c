// SPDX-License-Identifier: GPL-2.0
/*
 * Functions to read TDX global metadata.
 *
 * This file doesn't compile on its own as it lacks of inclusion
 * of SEAMCALL wrapper primitive which reads global metadata.
 * Include this file to other C file instead.
 */

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
