/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */

#ifndef _UECON_CHARDEV_H
#define _UECON_CHAR_H

#include <linux/miscdevice.h>

int uet_char_init(struct miscdevice *cdev, int id);
void uet_char_uninit(struct miscdevice *cdev);

#endif /* _UECON_CHARDEV_H */
