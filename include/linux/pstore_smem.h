#ifndef PSTORE_SMEM_H
#define PSTORE_SMEM_H

#include <linux/pstore_zone.h>

int register_pstore_smem_device(struct pstore_device_info *dev);
void unregister_pstore_smem_device(struct pstore_device_info *dev);

#endif
