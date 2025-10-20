// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2004 IBM Corporation
 * Authors:
 * Leendert van Doorn <leendert@watson.ibm.com>
 * Dave Safford <safford@watson.ibm.com>
 * Reiner Sailer <sailer@watson.ibm.com>
 * Kylene Hall <kjhall@us.ibm.com>
 *
 * Copyright (C) 2013 Obsidian Research Corp
 * Jason Gunthorpe <jgunthorpe@obsidianresearch.com>
 *
 * Device file system interface to the TPM
 */
#include <linux/slab.h>
#include "tpm-dev.h"

static int tpm_open(struct inode *inode, struct file *file)
{
	struct tpm_chip *chip;
	struct file_priv *priv;
	int rc;

	chip = container_of(inode->i_cdev, struct tpm_chip, cdev);

	/*
	 * It's assured that the chip will be opened just once via the direct
	 * /dev/tpm# interface by the check of is_open variable, which is
	 * protected by driver_lock.
	 */
	if (test_and_set_bit(0, &chip->is_open)) {
		dev_dbg(&chip->dev, "Another process owns this TPM\n");
		return -EBUSY;
	}

	/*
	 * If a client uses the O_EXCL flag then it expects to be the only TPM
	 * user across all access paths, so we treat it as a write lock.
	 * Otherwise we use a read lock, allowing for concurrent openers.
	 * We make sure the client is opening the device for reading + writing;
	 * opening for exclusive access doesn't make sense if not.
	 */
	if ((file->f_flags & (O_ACCMODE|O_EXCL)) == (O_RDWR|O_EXCL))
		rc = down_write_trylock(&chip->open_lock);
	else
		rc = down_read_trylock(&chip->open_lock);

	if (!rc) {
		dev_dbg(&chip->dev, "Another process owns this TPM\n");
		return -EBUSY;
	}

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (priv == NULL)
		goto out;
	priv->exclusive = (file->f_flags & O_EXCL);

	tpm_common_open(file, chip, priv, NULL);

	return 0;

 out:
	clear_bit(0, &chip->is_open);
	if (file->f_flags & O_EXCL)
		up_write(&chip->open_lock);
	else
		up_read(&chip->open_lock);
	return -ENOMEM;
}

/*
 * Called on file close
 */
static int tpm_release(struct inode *inode, struct file *file)
{
	struct file_priv *priv = file->private_data;

	tpm_common_release(file, priv);
	clear_bit(0, &priv->chip->is_open);
	if (priv->exclusive)
		up_write(&priv->chip->open_lock);
	else
		up_read(&priv->chip->open_lock);
	kfree(priv);

	return 0;
}

const struct file_operations tpm_fops = {
	.owner = THIS_MODULE,
	.open = tpm_open,
	.read = tpm_common_read,
	.write = tpm_common_write,
	.poll = tpm_common_poll,
	.release = tpm_release,
};
