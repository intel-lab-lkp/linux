/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __LIB_REFCOUNT_H
#define __LIB_REFCOUNT_H
bool percpu_ref_test_is_percpu(struct percpu_ref *ref);
void percpu_ref_test_flush_release_work(void);
#endif
