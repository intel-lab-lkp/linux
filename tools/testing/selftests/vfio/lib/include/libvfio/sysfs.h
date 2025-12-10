/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTESTS_VFIO_LIB_INCLUDE_LIBVFIO_SYSFS_H
#define SELFTESTS_VFIO_LIB_INCLUDE_LIBVFIO_SYSFS_H

int sysfs_get_sriov_totalvfs(const char *bdf);
int sysfs_get_sriov_numvfs(const char *bdf);
void sysfs_set_sriov_numvfs(const char *bdfs, int numvfs);
void sysfs_get_sriov_vf_bdf(const char *pf_bdf, int i, char *out_vf_bdf);
bool sysfs_get_sriov_drivers_autoprobe(const char *bdf);
void sysfs_set_sriov_drivers_autoprobe(const char *bdf, bool val);
void sysfs_bind_driver(const char *bdf, const char *driver);
void sysfs_unbind_driver(const char *bdf, const char *driver);
int sysfs_get_driver(const char *bdf, char *out_driver);
unsigned int sysfs_get_device_group(const char *bdf);

#endif /* SELFTESTS_VFIO_LIB_INCLUDE_LIBVFIO_SYSFS_H */
