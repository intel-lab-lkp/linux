// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef __BACKEND_CRYPTO_API_H__
#define __BACKEND_CRYPTO_API_H__

#include "zcomp.h"

struct zcomp_ops *get_backend_crypto_api(const char *name);

#endif /* __BACKEND_CRYPTO_API_H__ */
