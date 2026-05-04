#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

# Helper functions for creating extension headers using scapy

import ctypes
import shlex
import socket
import sys
import subprocess
import scapy
import proto_nums


# Read a sysctl
def sysctl_read(name):
    try:
        # shlex.split helps handle arguments correctly
        command = shlex.split(f"sysctl -n {name}")
        # Use check=True to raise an exception if the command fails
        result = subprocess.run(command, check=True,
                    capture_output=True, text=True)
        value = result.stdout.strip()
    except subprocess.CalledProcessError as ex:
        print(f"Error reading sysctl: {ex.stderr}")
    except FileNotFoundError:
        print("The 'sysctl' command was not found. "
              "Check your system's PATH.")

    return int(value)

# Common definitions for Destination and Hop-by-Hop options

# Common Destination and Hop-by-Hop Options header
class HbhDstOptions(ctypes.BigEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("next_hdr", ctypes.c_uint8),
        ("hdr_ext_len", ctypes.c_uint8)
    ]

    def __init__(self, next_hdr, length):
        self.next_hdr = next_hdr
        self.hdr_ext_len = length

# Common single Destination and Hop-by-Hop Option header
class HbhDstOption(ctypes.BigEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("opt_type", ctypes.c_uint8),
        ("opt_data_len", ctypes.c_uint8),
    ]

    def __init__(self, opt_type, length):
        self.opt_type = opt_type
        self.opt_data_len = length

# Make PAD1 option
def make_hbh_dst_option_pad1():
    opt_bytes = bytearray(1)
    opt_bytes[0] = proto_nums.HBHDst_Types.HBHDST_TYPE_PAD1.value
    return (scapy.all.Raw(opt_bytes), 1)

# Make a full DestOpt or HBH Option with some length
def make_hbh_dst_option_with_data(opt_type, opt_len):
    hdr = scapy.all.Raw(load=HbhDstOption(opt_type, opt_len))
    opt_bytes = scapy.all.Raw(bytearray(opt_len))
    allhdr = hdr/opt_bytes
    return (scapy.all.Raw(allhdr), 2 + opt_len)

# Make PADN option
def make_hbh_dst_option_pad_n(opt_len):
    return make_hbh_dst_option_with_data(
            proto_nums.HBHDst_Types.HBHDST_TYPE_PADN.value, opt_len)

# Make a Destination or Hop-by-Hop Options list. Input is list of pairs as
# (type, length). Option data is set to zeroes.
#
# Return value is (hdr, len, outcome) where hdr is the raw bytes and length
# is the length of the header including two bytes for the common extension
# header (the returned header does not include the two byte common header).
# outcome is True or False depending on whether the options are expected to
# exceed a sysctl limit and would be dropped
def make_hbh_dst_options_list(opt_list, max_cnt, max_len):
    hdr = scapy.all.Raw()
    eh_len = 0

    num_non_padding_opts = 0
    max_consect_pad_len = 0

    consect_padlen = 0

    # Create the set of options
    for opt_type, jlen in opt_list:
        if opt_type == proto_nums.HBHDst_Types.HBHDST_TYPE_PAD1.value:
            # PAD1 is a special case
            pair = make_hbh_dst_option_pad1()
            consect_padlen += pair[1]
        else:
            pair = make_hbh_dst_option_with_data(opt_type, jlen)

            if opt_type == proto_nums.HBHDst_Types.HBHDST_TYPE_PADN.value:
                consect_padlen += pair[1]
            else:
                if consect_padlen > max_consect_pad_len:
                    max_consect_pad_len = consect_padlen
                consect_padlen = 0
                num_non_padding_opts += 1

        # Append the option, add to cumulative length
        hdr = hdr/pair[0]
        eh_len += pair[1]

    # Add two to length to account for two byte extension header
    eh_len += 2

    if eh_len % 8 != 0:
        # The extension header length must be a multiple of eight bytes.
        # If we're short add a padding option
        plen = 8 - (eh_len % 8)
        if plen == 1:
            pair = make_hbh_dst_option_pad1()
        else:
            pair = make_hbh_dst_option_pad_n(plen - 2)

        consect_padlen += pair[1]
        hdr = hdr/pair[0]
        eh_len += plen

    if consect_padlen > max_consect_pad_len:
        max_consect_pad_len = consect_padlen

    outcome = True
    if num_non_padding_opts > max_cnt:
        # The number of options we created is greater then the sysctl
        # limit, so we expect the packet to be dropped
        outcome = False
    if eh_len > max_len:
        # The length of the extension is greater then the sysctl limit,
        # so we expect the packet to be dropped
        outcome = False
    if max_consect_pad_len > 7:
        # The maximum consecutive number of bytes of padding is
        # greater than seven, so we expect the packet to be dropped
        outcome = False

    return (hdr, eh_len - 2, outcome)

# Make a full Hop-by-Hop or Destination Options header
def make_full_hbh_dst_options_list(next_hdr, opt_list, max_cnt, max_len):
    pair = make_hbh_dst_options_list(opt_list, max_cnt, max_len)
    opt_len = pair[1] + 2

    opts = HbhDstOptions(next_hdr, (opt_len - 1) // 8)
    hdr = scapy.all.Raw(load=opts)/pair[0]

    return (hdr, opt_len, pair[2])

# Routing header definitions

# Base Routing Header
class RoutingHdr(ctypes.BigEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("next_hdr", ctypes.c_uint8),
        ("hdr_ext_len", ctypes.c_uint8),
        ("routing_type", ctypes.c_uint8),
        ("segments_left", ctypes.c_uint8)
    ]

# SRv6 Routing Header
class Srv6RoutingHdr(ctypes.BigEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("rh", RoutingHdr),
        ("last_entry", ctypes.c_uint8),
        ("flags", ctypes.c_uint8),
        ("tags", ctypes.c_uint16),
        # Variable list
        # TLV options
   ]

    def __init__(self, next_hdr, hdr_ext_len, segments_left, last_entry):
        self.rh.next_hdr = next_hdr
        self.rh.hdr_ext_len = hdr_ext_len
        self.rh.routing_type = proto_nums.RoutingTypes.ROUTING_TYPE_SRH.value
        self.rh.segments_left = segments_left

        self.last_entry = last_entry

# Make an SRv6 Routing Header (with no segments left)
def make_srv6_routing_hdr(next_hdr, sids):

    bhdr = scapy.all.Raw()
    num_sids = 0

    # Set up each SID in the list
    for sid in sids:
        sid_bytes = socket.inet_pton(socket.AF_INET6, sid)
        bhdr = bhdr/scapy.all.Raw(load=sid_bytes)
        num_sids += 1

    eh_len = num_sids * 16

    hdr = Srv6RoutingHdr(next_hdr, eh_len // 8, 0, num_sids - 1)

    bhdr = scapy.all.Raw(load=hdr)/bhdr

    return (bhdr, eh_len + 8, True)

# Fragment header

# Basic Fragment Header
class FragmentHdr(ctypes.BigEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("next_hdr", ctypes.c_uint8),
        ("rsvd", ctypes.c_uint8),
        ("fragment_offset", ctypes.c_uint16, 13),
        ("rsvd2", ctypes.c_uint16, 2),
        ("more", ctypes.c_uint16, 1),
        ("identfication", ctypes.c_uint32),
    ]

    def __init__(self, next_hdr, fragment_offset, more, ident):
        self.next_hdr = next_hdr
        self.fragment_offset = fragment_offset
        self.more = more
        self.identfication = ident

# Make a raw fragment header
def make_fragment_hdr(next_hdr, fragment_offset, more, ident):
    hdr = FragmentHdr(next_hdr, fragment_offset, more, ident)

    return (scapy.all.Raw(load=hdr), 8, True)

# Authentication Header

# Base Authentication Header
class AuthHdr(ctypes.BigEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("next_hdr", ctypes.c_uint8),
                ("payload_len", ctypes.c_uint8),
        ("spi", ctypes.c_uint32)
        # ICV is variable length
    ]

    def __init__(self, next_hdr, payload_len, spi):
        self.next_hdr = next_hdr
        self.payload_len = payload_len
        self.spi = spi

# ESP

# Base ESP header
class EspHdr(ctypes.BigEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("spi", ctypes.c_uint32),
        ("seqno", ctypes.c_uint32)
        # Payload data + padding
        # ICV is variable length
    ]

    def __init__(self, spi, seqno):
        self.spi = spi
        self.seqno = seqno

# Check if EH list is out of order
def check_eh_order(eh_list):
    # OOO is okay if sysctl is not enforcing in order
    do_check = sysctl_read("net.ipv6.enforce_ext_hdr_order")

    seen = 0
    for eh_type, _args in eh_list:
        if eh_type == "H":
            order = proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_HOP.value
        elif eh_type == "D":
            if (seen &
                proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_ROUTING.value):
                order = proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_DEST.value
            else:
                order = proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_DEST_BEFORE_RH.value
        elif eh_type == "R":
            order = proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_ROUTING.value
        elif eh_type == "F":
            order = proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_FRAGMENT.value
            if seen & order != 0:
                # Linux stack doesn't allow more than one
                # Fragment Header in a packet
                return False
        elif eh_type == "A":
            order = proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_AUTH.value
        elif eh_type == "E":
            order = proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_ESP.value

        if (do_check and seen >= order):
            return False
        seen |= order

    return True

# Compute the next headers for an EH chain. Returns a new list of EHs
# with the next header attached to each element
def compute_next_hdrs(next_hdr, eh_list):
    nlist = []

    # Run through the list in reverse and set the next header up for each
    # enty
    for eh_type, args in reversed(eh_list):
        entry = (eh_type, args, next_hdr)
        nlist.insert(0, entry)
        if eh_type == "H":
            next_hdr = proto_nums.IP_Proto.IP_PROTO_HOPOPT.value
        elif eh_type == "D":
            next_hdr = proto_nums.IP_Proto.IP_PROTO_IPv6_Opts.value
        elif eh_type == "R":
            next_hdr = proto_nums.IP_Proto.IP_PROTO_IPv6_Route.value
        elif eh_type == "F":
            next_hdr = proto_nums.IP_Proto.IP_PROTO_IPv6_Frag.value
        elif eh_type == "A":
            next_hdr = proto_nums.IP_Proto.IP_PROTO_AH.value
        elif eh_type == "E":
            next_hdr = proto_nums.IP_Proto.IP_PROTO_ESP.value

    return nlist, next_hdr

# Make an extension header chain from a list
# The list contains a set of pairs in the form (<eh_type>, <args>)
# <eh_type> is:
#   "H"-- Hop-by-Hop Options
#   "D"-- Destination Options
#   "R"-- Routing Header
#   "F"-- Fragment Header
#   "A"-- Authentication Header
#   "E"-- ESP
#
# <args> is specific to EH type
def make_eh_chain(next_hdr, eh_list):
    nlist = []

    # Run through the list in reverse and set the next header up for each
    # enty
    nlist, next_hdr = compute_next_hdrs(next_hdr, eh_list)

    outcome = check_eh_order(eh_list)

    hdr = scapy.all.Raw()
    eh_len = 0

    for eh_type, args, nnext_hdr in reversed(nlist):
        if eh_type == "H":
            # args is a list of (<opt_type>, <opt_len>) pairs
            pair = make_full_hbh_dst_options_list(nnext_hdr, args,
                sysctl_read("net.ipv6.max_hbh_opts_number"),
                sysctl_read("net.ipv6.max_hbh_length"))
        elif eh_type == "D":
            # args is a list of (<opt_type>, <opt_len>) pairs
            pair = make_full_hbh_dst_options_list(nnext_hdr, args,
                sysctl_read("net.ipv6.max_dst_opts_number"),
                sysctl_read("net.ipv6.max_dst_opts_length"))
        elif eh_type == "R":
            # args is a list of IPv6 address string
            pair = make_srv6_routing_hdr(nnext_hdr, args)
        elif eh_type == "F":
            # Arg is (<identifier>)
            pair = make_fragment_hdr(nnext_hdr, 0, False, args)
        elif eh_type == "A":
            print("Auth type not supported for test")
            sys.exit(1)
        elif eh_type == "E":
            print("ESP type not supported for test")
            sys.exit(1)
        else:
            print("Unknown EH type character")
            sys.exit(1)

        hdr = pair[0]/hdr
        eh_len += pair[1]

        if pair[2] is False:
            outcome = False

    return (hdr, eh_len, next_hdr, outcome)
