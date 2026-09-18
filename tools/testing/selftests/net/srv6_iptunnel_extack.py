#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# author: Gabriel Goller <g.goller@proxmox.com>

"""Exercise SRv6 tunnel validation with invalid netlink attributes."""

import errno
import os
import socket
import struct
from contextlib import contextmanager

from lib.py import KsftNamedVariant, KsftSkipEx, Netlink, NetNS, NetNSEnter
from lib.py import NlError, RtnlRouteFamily, ip, ksft_eq, ksft_exit, ksft_raises
from lib.py import ksft_run, ksft_variants


LWTUNNEL_ENCAP_SEG6 = 5
SEG6_IPTUNNEL_SRH = 1
SEG6_IPTUNNEL_SRC = 2
SEG6_IPTUNNEL_TABLE = 3
SEG6_IPTUN_MODE_INLINE = 0
SEG6_IPTUN_MODE_ENCAP = 1
SEG6_IPTUN_MODE_L2ENCAP = 2
SEG6_IPTUN_MODE_ENCAP_RED = 3
SEG6_IPTUN_MODE_L2ENCAP_RED = 4


def encap(mode=SEG6_IPTUN_MODE_ENCAP, hdrlen=2, routing_type=4):
    # seg6_iptunnel_encap followed by an SRH containing one segment.
    srh = struct.pack('!BBBBBBH', 0, hdrlen, routing_type, 0, 0, 0, 0)
    srh += socket.inet_pton(socket.AF_INET6, '2001:db8::1')
    return struct.pack('=i', mode) + srh


def nlattr(attr_type, payload):
    length = 4 + len(payload)
    return (struct.pack('=HH', length, attr_type) + payload +
            bytes(-length % 4))


def route(family, oif, payload, tunsrc=None, table=None):
    attrs = b''
    if payload is not None:
        attrs += nlattr(SEG6_IPTUNNEL_SRH, payload)
    if tunsrc is not None:
        attrs += nlattr(SEG6_IPTUNNEL_SRC,
                        socket.inet_pton(socket.AF_INET6, tunsrc))
    if table is not None:
        attrs += nlattr(SEG6_IPTUNNEL_TABLE, struct.pack('=I', table))

    return {
        'rtm-family': family,
        'rtm-dst-len': 32 if family == socket.AF_INET else 128,
        'rtm-table': 254,
        'rtm-protocol': 4,  # RTPROT_STATIC
        'rtm-type': 1,  # RTN_UNICAST
        'dst': '192.0.2.1' if family == socket.AF_INET else '2001:db8:1::1',
        'oif': oif,
        'encap-type': LWTUNNEL_ENCAP_SEG6,
        'encap': attrs,
    }


def add_route(rtnl, attrs):
    rtnl.newroute(attrs.copy(),
                  flags=[Netlink.NLM_F_CREATE, Netlink.NLM_F_EXCL])


def matching_routes(rtnl, attrs):
    routes = rtnl.getroute({'rtm-family': attrs['rtm-family']}, dump=True)
    return [entry for entry in routes
            if entry.get('dst') == attrs['dst'] and
            entry['rtm-table'] == attrs['rtm-table']]


@contextmanager
def setup():
    if os.geteuid() != 0:
        raise KsftSkipEx('Root privileges are required')

    with NetNS() as ns, NetNSEnter(str(ns)):
        ip('link set lo up')
        oif = socket.if_nametoindex('lo')
        rtnl = RtnlRouteFamily()
        try:
            # Probe with a valid route so missing SRv6 support is a skip,
            # whereas missing diagnostics remain a test failure.
            probe = route(socket.AF_INET6, oif, encap())
            try:
                add_route(rtnl, probe)
            except NlError as error:
                if error.error == errno.EOPNOTSUPP:
                    raise KsftSkipEx('SRv6 tunnels are not supported') from error
                raise
            rtnl.delroute(probe.copy())
            yield rtnl, oif
        finally:
            rtnl.close()


# (name, SRH payload, extra encap attributes, expected extack message)
INVALID = [
    ('missing_srh', None, {}, 'missing SRv6 SRH attribute'),
    ('empty_srh', b'', {}, 'truncated SRv6 SRH attribute'),
    ('short_srh', encap()[:-1], {}, 'truncated SRv6 SRH attribute'),
    ('invalid_mode', encap(mode=255), {}, 'invalid SRv6 encapsulation mode'),
    ('invalid_type', encap(routing_type=0), {},
     'invalid SRv6 segment routing header'),
    ('invalid_length', encap(hdrlen=4), {},
     'invalid SRv6 segment routing header'),
    ('invalid_tunsrc', encap(), {'tunsrc': '::'}, 'invalid tunsrc address'),
    ('invalid_table', encap(), {'table': 0}, 'invalid lookup table'),
]


@ksft_variants([
    KsftNamedVariant(f'{name}_{af_name}', family, payload, extra, message)
    for name, payload, extra, message in INVALID
    for af_name, family in [('ipv4', socket.AF_INET), ('ipv6', socket.AF_INET6)]
] + [KsftNamedVariant('inline_ipv4', socket.AF_INET,
                      encap(mode=SEG6_IPTUN_MODE_INLINE), {},
                      'inline mode requires an IPv6 route'),
     # Inline mode rejects a non-IPv6 route before it looks at tunsrc,
     # so only IPv6 reaches the tunsrc check.
     KsftNamedVariant('inline_tunsrc_ipv6', socket.AF_INET6,
                      encap(mode=SEG6_IPTUN_MODE_INLINE),
                      {'tunsrc': '2001:db8::2'},
                      'incompatible mode for tunsrc')])
def invalid_config(family, payload, extra, message):
    with setup() as (rtnl, oif):
        attrs = route(family, oif, payload, **extra)
        with ksft_raises(NlError) as caught:
            add_route(rtnl, attrs)
        ksft_eq(matching_routes(rtnl, attrs), [], 'Rejected route was installed')
        if caught.exception is None:
            return
        ksft_eq(caught.exception.error, errno.EINVAL)
        ksft_eq((caught.exception.nl_msg.extack or {}).get('msg'), message)


@ksft_variants([
    KsftNamedVariant(f'{name}_{af_name}', family, mode)
    for name, mode in [('inline', SEG6_IPTUN_MODE_INLINE),
                       ('encap', SEG6_IPTUN_MODE_ENCAP),
                       ('l2encap', SEG6_IPTUN_MODE_L2ENCAP),
                       ('encap_red', SEG6_IPTUN_MODE_ENCAP_RED),
                       ('l2encap_red', SEG6_IPTUN_MODE_L2ENCAP_RED)]
    for af_name, family in [('ipv4', socket.AF_INET), ('ipv6', socket.AF_INET6)]
    if mode != SEG6_IPTUN_MODE_INLINE or family == socket.AF_INET6
])
def valid_config(family, mode):
    with setup() as (rtnl, oif):
        attrs = route(family, oif, encap(mode=mode))
        add_route(rtnl, attrs)
        routes = matching_routes(rtnl, attrs)
        ksft_eq(len(routes), 1)
        if routes:
            ksft_eq(routes[0].get('encap-type'), LWTUNNEL_ENCAP_SEG6)
            ksft_eq(routes[0].get('encap'), attrs['encap'])
            rtnl.delroute(attrs.copy())
            ksft_eq(matching_routes(rtnl, attrs), [])


if __name__ == '__main__':
    ksft_run([invalid_config, valid_config])
    ksft_exit()
