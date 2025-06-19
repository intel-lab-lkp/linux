/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __IP_SET_HASH_H
#define __IP_SET_HASH_H

#include <uapi/linux/netfilter/ipset/ip_set_hash.h>


#define IPSET_DEFAULT_HASHSIZE		1024
#define IPSET_MINIMAL_HASHSIZE		64
/* Legacy alias for the old typo – keep until v6.1 LTS (EOL: 2027-12-31) */
#define IPSET_MIMINAL_HASHSIZE		IPSET_MINIMAL_HASHSIZE
#define IPSET_DEFAULT_MAXELEM		65536
#define IPSET_DEFAULT_PROBES		4
#define IPSET_DEFAULT_RESIZE		100

#endif /* __IP_SET_HASH_H */
