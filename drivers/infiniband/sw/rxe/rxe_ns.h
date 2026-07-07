/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */

#ifndef RXE_NS_H
#define RXE_NS_H

/*
 * Factory used to create a shared per-namespace tunnel socket while the
 * pernet lock is held. It must return:
 *   - a valid sk on success,
 *   - NULL if the address family is unsupported (not treated as an error),
 *   - an ERR_PTR() on failure.
 */
typedef struct sock *(*rxe_sk_create_t)(struct net *net);

struct sock *rxe_ns_pernet_hold_sk4(struct net *net, rxe_sk_create_t create);
void rxe_ns_pernet_put_sk4(struct net *net);

#if IS_ENABLED(CONFIG_IPV6)
struct sock *rxe_ns_pernet_sk6(struct net *net);
struct sock *rxe_ns_pernet_hold_sk6(struct net *net, rxe_sk_create_t create);
void rxe_ns_pernet_put_sk6(struct net *net);
#else /* IPv6 */
static inline struct sock *rxe_ns_pernet_sk6(struct net *net)
{
	return NULL;
}

static inline struct sock *rxe_ns_pernet_hold_sk6(struct net *net,
						  rxe_sk_create_t create)
{
	return NULL;
}

static inline void rxe_ns_pernet_put_sk6(struct net *net)
{
}
#endif /* IPv6 */

int rxe_namespace_init(void);
void rxe_namespace_exit(void);

#endif /* RXE_NS_H */
