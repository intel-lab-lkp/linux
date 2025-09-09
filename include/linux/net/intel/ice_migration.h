/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2018-2025 Intel Corporation */

#ifndef _ICE_MIGRATION_H_
#define _ICE_MIGRATION_H_

#if IS_ENABLED(CONFIG_ICE_VFIO_PCI)
int ice_migration_init_dev(struct pci_dev *vf_dev);
void ice_migration_uninit_dev(struct pci_dev *vf_dev);
size_t ice_migration_get_required_size(struct pci_dev *vf_dev);
int ice_migration_save_devstate(struct pci_dev *vf_dev, void *buf,
				size_t buf_sz);
int ice_migration_load_devstate(struct pci_dev *vf_dev,
				const void *buf, size_t buf_sz);
int ice_migration_suspend_dev(struct pci_dev *vf_dev, bool save_state);
#else
static inline int ice_migration_init_dev(struct pci_dev *vf_dev)
{
	return -EOPNOTSUPP;
}

static inline void ice_migration_uninit_dev(struct pci_dev *vf_dev) { }

static inline size_t ice_migration_get_required_size(struct pci_dev *vf_dev)
{
	return 0;
}

static inline int
ice_migration_save_devstate(struct pci_dev *vf_dev, void *buf,
			    size_t buf_sz)
{
	return -EOPNOTSUPP;
}

static inline int ice_migration_load_devstate(struct pci_dev *vf_dev,
					      const void *buf, size_t buf_sz)
{
	return -EOPNOTSUPP;
}

static inline int ice_migration_suspend_dev(struct pci_dev *vf_dev,
					    bool save_state)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_ICE_VFIO_PCI */

#endif /* _ICE_MIGRATION_H_ */
