/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

#ifndef _UAPI__LINUX_GN_H__
#define _UAPI__LINUX_GN_H__

#include <linux/types.h>
#include <asm/byteorder.h>
#include <linux/socket.h>

#include <linux/time_types.h>

/*
 * GeoNetworking structures
 *
 */
#define GNPORT_ANY	0
#define GNPORT_FIRST	1
#define GNPORT_RESERVED	3000
#define GNPORT_LAST	65535

#define GNADDR_BROADCAST 0x0000FFFFFFFFFFFFLLU

/* Valid protocols */
#define GN_PROTO_ANY	0
#define GN_PROTO_BTP_A	1
#define GN_PROTO_BTP_B	2
#define GN_PROTO_INET6	3
#define GN_PROTO_MAX	GN_PROTO_INET6

/* protocol-specific ioctls */
#define SIOCGNSPOSITION			(SIOCPROTOPRIVATE + 0)

/* gn_position flags */
#define GN_POSITION_MOBILE		(1 << 0)

/* {get,set}sockopt options */
#define GN_SCOPE 1

#define GN_SCOPE_UNSPECIFIED		0
#define GN_SCOPE_TOPOLOGICAL		1
#define GN_SCOPE_GEOGRAPHICAL		2
#define GN_SCOPE_GEOGRAPHICAL_ANYCAST	3
#define GN_SCOPE_MAX			GN_SCOPE_GEOGRAPHICAL_ANYCAST

#define GN_SHAPE_UNSPECIFIED	0
#define GN_SHAPE_CIRCLE		1
#define GN_SHAPE_ELLIPSE	2
#define GN_SHAPE_RECTANGLE	3

typedef __be64 gn_address_t;

struct gn_coord {
	__s32 lat;
	__s32 lon;
};

struct gn_geo_scope {
	struct gn_coord coord;
	__u16 angle;
	__u16 a, b;
	__u8 shape;
};

struct gn_scope {
	__u8 scope_type;
	union {
		__u8 topo_hops;
		struct gn_geo_scope geo_scope;
	};
};

struct gn_position {
	struct __kernel_timespec tst;
	struct gn_coord coord;
	__u8 flags;
};

struct sockaddr_gn {
	__kernel_sa_family_t sgn_family;
	gn_address_t sgn_addr;
	__u16 sgn_port;
} __packed;

#endif /* _UAPI__LINUX_GN_H__ */
