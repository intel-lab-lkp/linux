/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EIP93_FALLBACK_H_
#define _EIP93_FALLBACK_H_

struct eip93_device;

int eip93_fallback_register(struct eip93_device *eip93);
void eip93_fallback_unregister(void);

#endif /* _EIP93_FALLBACK_H_ */
