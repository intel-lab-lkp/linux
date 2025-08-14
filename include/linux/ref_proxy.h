/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINUX_REF_PROXY_H
#define __LINUX_REF_PROXY_H

#include <linux/cleanup.h>

struct device;
struct ref_proxy;
struct ref_proxy_provider;

struct ref_proxy_provider *ref_proxy_provider_alloc(void *ref);
void ref_proxy_provider_free(struct ref_proxy_provider *rpp);
struct ref_proxy_provider *devm_ref_proxy_provider_alloc(struct device *dev,
							 void *ref);

struct ref_proxy *ref_proxy_alloc(struct ref_proxy_provider *rpp);
void ref_proxy_free(struct ref_proxy *proxy);
void __rcu *ref_proxy_get(struct ref_proxy *proxy);
void ref_proxy_put(struct ref_proxy *proxy);

DEFINE_FREE(ref_proxy, struct ref_proxy *, if (_T) ref_proxy_put(_T))

#define _REF_PROXY_GET(_proxy, _name, _label, _ref) \
	for (struct ref_proxy *_name __free(ref_proxy) = _proxy;	\
	     (_ref = ref_proxy_get(_name)) || true; ({ goto _label; }))	\
		if (0) {						\
_label:									\
			break;						\
		} else

#define REF_PROXY_GET(_proxy, _ref)					\
	_REF_PROXY_GET(_proxy, __UNIQUE_ID(proxy_name),			\
		       __UNIQUE_ID(label), _ref)

#endif /* __LINUX_REF_PROXY_H */

