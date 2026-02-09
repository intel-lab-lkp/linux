/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MISC_ASUS_EC_H
#define __MISC_ASUS_EC_H

struct i2c_client;

#define DOCKRAM_ENTRIES			0x100
#define DOCKRAM_ENTRY_SIZE		32
#define DOCKRAM_ENTRY_BUFSIZE		(DOCKRAM_ENTRY_SIZE + 1)

/* control register [0x0A] layout */
#define ASUSEC_CTL_SIZE			8

#define ASUSEC_DOCKRAM_CONTROL		0x0a

/* dockram comm */
int asus_dockram_read(struct i2c_client *client, int reg, char *buf);
int asus_dockram_write(struct i2c_client *client, int reg, const char *buf);
int asus_dockram_access_ctl(struct i2c_client *client,
			    u64 *out, u64 mask, u64 xor);
struct i2c_client *devm_asus_dockram_get(struct device *parent);
void asus_dockram_debugfs_init(struct i2c_client *client,
			       struct dentry *debugfs_root);
#endif /* __MISC_ASUS_EC_H */
