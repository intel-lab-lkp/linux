#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

# Helper functions for creating extension headers using scapy

import ctypes
import os
import proto_nums
import shlex
import socket
import sys
from enum import Enum
from scapy.all import *

# Read a sysctl
def SysctlRead(name):
	try:
		# shlex.split helps handle arguments correctly
		command = shlex.split("sysctl -n %s" % (name))
		# Use check=True to raise an exception if the command fails
		result = subprocess.run(command, check=True,
					capture_output=True, text=True)
		value = result.stdout.strip()
	except subprocess.CalledProcessError as e:
		print(f"Error reading sysctl: {e.stderr}")
	except FileNotFoundError:
		print("The 'sysctl' command was not found. "
		      "Check your system's PATH.")

	return int(value)

# Common definitions for Destination and Hop-by-Hop options

# Common Destination and Hop-by-Hop Options header
class hbh_dst_options(ctypes.BigEndianStructure):
	_pack_ = 1
	_fields_ = [
		("next_hdr", ctypes.c_uint8),
		("hdr_ext_len", ctypes.c_uint8)
	]

# Make a common Destination and Hop-by-Hop Options header
def Make_HBHDst_Options(next_hdr, length):
	hdr = hbh_dst_options()
	hdr.next_hdr = next_hdr
	hdr.hdr_ext_len = length

	return hdr

# Common single Destination and Hop-by-Hop Option header
class dst_hbh_option(ctypes.BigEndianStructure):
	_pack_ = 1
	_fields_ = [
		("opt_type", ctypes.c_uint8),
		("opt_data_len", ctypes.c_uint8),
	]

# Make common single Destination and Hop-by-Hop Option header
def Make_HBHDst_Option(type, length):
	hdr = dst_hbh_option()
	hdr.opt_type = type
	hdr.opt_data_len = length

	return hdr

# Make PAD1 option
def Make_HBHDst_Option_pad1():
	bytes = bytearray(1)
	bytes[0] = proto_nums.HBHDst_Types.HBHDST_TYPE_PAD1.value
	return (Raw(bytes), 1)

# Make a full DestOpt or HBH Option with some length
def Make_HBHDst_Option_with_Data(type, len):
	hdr = Raw(load=Make_HBHDst_Option(type, len))
	bytes = Raw(bytearray(len))
	allhdr = hdr/bytes
	return (allhdr, 2 + len)

# Make PADN option
def Make_HBHDst_Option_padN(len):
	return Make_HBHDst_Option_with_Data(
			proto_nums.HBHDst_Types.HBHDST_TYPE_PADN.value, len)

# Make a Destination or Hop-by-Hop Options list. Input is list of pairs as
# (type, length). Option data is set to zeroes.
#
# Return value is (hdr, len, outcome) where hdr is the raw bytes and length
# is the length of the header including two bytes for the common extension
# header (the returned header does not include the two byte common header).
# outcome is True or False depending on whether the options are expected to
# exceed a sysctl limit and would be dropped
def Make_HBHDst_Options_list(list, max_cnt, max_len):
	hdr = Raw()
	len = 0

	num_non_padding_opts = 0
	max_consect_pad1 = 0
	max_consect_pad_len = 0

	consect_padlen = 0

	# Create the set of options
	for type, jlen in list:
		if (type == proto_nums.HBHDst_Types.HBHDST_TYPE_PAD1.value):
			# PAD1 is a special case
			pair = Make_HBHDst_Option_pad1()
			consect_padlen += pair[1]
		else:
			pair = Make_HBHDst_Option_with_Data(type, jlen)

			if (type ==
			    proto_nums.HBHDst_Types.HBHDST_TYPE_PADN.value):
				consect_padlen += pair[1]
			else:
				if (consect_padlen > max_consect_pad_len):
					max_consect_pad_len = consect_padlen
				consect_padlen = 0
				num_non_padding_opts += 1

		# Append the option, add to cumulative length
		hdr = hdr/pair[0]
		len += pair[1]

	# Add two to length to account for two byte extension header
	len += 2

	if (len % 8 != 0):
		# The extension header length must be a multiple of eight bytes.
		# If we're short add a padding option
		plen = 8 - (len % 8)
		if (plen == 1):
			pair = Make_HBHDst_Option_pad1()
		else:
			pair = Make_HBHDst_Option_padN(plen - 2)

		consect_padlen += pair[1]
		hdr = hdr/pair[0]
		len += plen

	if (consect_padlen > max_consect_pad_len):
		max_consect_pad_len = consect_padlen

	outcome = True
	if (num_non_padding_opts > max_cnt):
		# The number of options we created is greater then the sysctl
		# limit, so we expect the packet to be dropped
		outcome = False
	if (len > max_len):
		# The length of the extension is greater then the sysctl limit,
		# so we expect the packet to be dropped
		outcome = False
	if (max_consect_pad_len > 7):
		# The maximum consecutive number of bytes of padding is
		# greater than seven, so we expect the packet to be dropped
		outcome = False

	return (hdr, len - 2, outcome)

# Make a full Hop-by-Hop or Destination Options header
def Make_Full_HBHDst_Options_list(next_hdr, list, max_cnt, max_len):
	pair = Make_HBHDst_Options_list(list, max_cnt, max_len)
	len = pair[1] + 2

	opts = Make_HBHDst_Options(next_hdr, (len - 1) // 8)
	hdr = Raw(load=opts)/pair[0]

	return (hdr, len, pair[2])

# Routing header definitions

# Base Routing Header
class routing_hdr(ctypes.BigEndianStructure):
	_pack_ = 1
	_fields_ = [
		("next_hdr", ctypes.c_uint8),
		("hdr_ext_len", ctypes.c_uint8),
		("routing_type", ctypes.c_uint8),
		("segments_left", ctypes.c_uint8)
	]

# SRv6 Routing Header
class srv6_routing_hdr(ctypes.BigEndianStructure):
	_pack_ = 1
	_fields_ = [
		("rh", routing_hdr),
		("last_entry", ctypes.c_uint8),
		("flags", ctypes.c_uint8),
		("tags", ctypes.c_uint16),
		# Variable list
		# TLV options
   ]

# Make a basic Routing header
def MakeRouting_Hdr(next_hdr, hdr_ext_len, routing_type, segments_left):
	hdr = routing_hdr()
	hdr.next_hdr = next_hdr
	hdr.hdr_ext_len = hdr_ext_len
	hdr.routing_type = routing_type
	hdr.segments_left = segments_left

	return Raw(load=hdr)

# Make an SRv6 Routing Header (with no segments left)
def MakeSRv6Routing_Hdr(next_hdr, sids, tlvs):

	bhdr = Raw()
	num_sids = 0

	# Set up each SID in the list
	for sid in sids:
		bytes = socket.inet_pton(socket.AF_INET6, sid)
		bhdr = bhdr/Raw(load=bytes)
		num_sids += 1

	len = num_sids * 16

	hdr = srv6_routing_hdr()

	hdr.rh.next_hdr = next_hdr
	hdr.rh.hdr_ext_len = len // 8
	hdr.rh.routing_type = proto_nums.RoutingTypes.ROUTING_TYPE_SRH.value
	hdr.rh.segments_left = 0

	hdr.last_entry = num_sids - 1
	hdr.flags = 0
	hdr.tags = 0

	bhdr = Raw(load=hdr)/bhdr

	return (bhdr, len + 8, True)

# Fragment header

# Basic Fragment Header
class fragment_hdr(ctypes.BigEndianStructure):
	_pack_ = 1
	_fields_ = [
		("next_hdr", ctypes.c_uint8),
		("rsvd", ctypes.c_uint8),
		("fragment_offset", ctypes.c_uint16, 13),
		("rsvd2", ctypes.c_uint16, 2),
		("more", ctypes.c_uint16, 1),
		("identfication", ctypes.c_uint32),
	]

# Make Fragment Header
def MakeFragment_Hdr(next_hdr, fragment_offset, more, ident):
	hdr = fragment_hdr()
	hdr.next_hdr = next_hdr
	hdr.fragment_offset = fragment_offset
	hdr.identfication = ident

	return (Raw(load=hdr), 8, True)

# Authentication Header

# Base Authentication Header
class auth_hdr(ctypes.BigEndianStructure):
	_pack_ = 1
	_fields_ = [
		("next_hdr", ctypes.c_uint8),
                ("payload_len", ctypes.c_uint8),
		("spi", ctypes.c_uint32)
		# ICV is variable length
	]

# Make and authenrication Header
def MakeAuth_Hdr(next_hdr, payload_len, spi):
	hdr = auth_hdr()
	hdr.next_hdr = next_hdr
	hdr.payload_len = payload_len
	hdr.spi = spi

	return hdr

# ESP

# Base ESP header
class esp_hdr(ctypes.BigEndianStructure):
	_pack_ = 1
	_fields_ = [
		("spi", ctypes.c_uint32),
		("seqno", ctypes.c_uint32)
		# Payload data + padding
		# ICV is variable length
	]

# Make an ESP header
def MakeESP_Hdr(next_hdr, spi, seqno):
	hdr = esp_hdr()
	hdr.spi = spi
	hdr.seqno = seqno

	return hdr

# Check if EH list is out of order
def Check_EH_Order(list):
	# OOO is okay if sysctl is not enforcing in order
	do_check = SysctlRead("net.ipv6.enforce_ext_hdr_order")

	seen = 0
	for type, args in list:
		if type == "H":
			order = proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_HOP.value
		elif type == "D":
			if (seen &
			    proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_ROUTING.value):
				order = proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_DEST.value
			else:
				order = proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_DEST_BEFORE_RH.value
		elif type == "R":
			order = proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_ROUTING.value
		elif type == "F":
			order = proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_FRAGMENT.value
			if (seen & order != 0):
				# Linux stack doesn't allow more than one
				# Fragment Header in a packet
				return False
		elif type == "A":
			order = proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_AUTH.value
		elif type == "E":
			order = proto_nums.EH_Order.IPV6_EXT_HDR_ORDER_ESP.value

		if (do_check and seen >= order):
			return False
		seen |= order

	return True

# Make an extension header chain from a list
# The list contains a set of pairs in the form (<eh_type>, <args>)
# <eh_type> is:
#	"H"-- Hop-by-Hop Options
#	"D"-- Destination Options
#	"R"-- Routing Header
#	"F"-- Fragment Header
#	"A"-- Authentication Header
#	"E"-- ESP
#
# <args> is specific to EH type
def Make_EH_Chain(next_hdr, list):
	nlist = []

	# Run through the list in reverse and set the next header up for each
	# enty
	for type, args in reversed(list):
		entry = (type, args, next_hdr)
		nlist.insert(0, entry)
		if type == "H":
			next_hdr = proto_nums.IP_Proto.IP_PROTO_HOPOPT.value
		elif type == "D":
			next_hdr = proto_nums.IP_Proto.IP_PROTO_IPv6_Opts.value
		elif type == "R":
			next_hdr = proto_nums.IP_Proto.IP_PROTO_IPv6_Route.value
		elif type == "F":
			next_hdr = proto_nums.IP_Proto.IP_PROTO_IPv6_Frag.value
		elif type == "A":
			next_hdr = proto_nums.IP_Proto.IP_PROTO_AH.value
		elif type == "E":
			next_hdr = proto_nums.IP_Proto.IP_PROTO_ESP.value

	outcome = Check_EH_Order(list)

	hdr = Raw()
	len = 0

	for type, args, nnext_hdr in reversed(nlist):
		if type == "H":
			# args is a list of (<opt_type>, <opt_len>) pairs
			pair = Make_Full_HBHDst_Options_list(nnext_hdr, args,
				SysctlRead("net.ipv6.max_hbh_opts_number"),
				SysctlRead("net.ipv6.max_hbh_length"))
		elif type == "D":
			# args is a list of (<opt_type>, <opt_len>) pairs
			pair = Make_Full_HBHDst_Options_list(nnext_hdr, args,
				SysctlRead("net.ipv6.max_dst_opts_number"),
				SysctlRead("net.ipv6.max_dst_opts_length"))
		elif type == "R":
			# args is a list of IPv6 address string
			pair = MakeSRv6Routing_Hdr(nnext_hdr, args, [])
		elif type == "F":
			# Arg is (<identifier>)
			pair = MakeFragment_Hdr(nnext_hdr, 0, False, args)
		elif type == "A":
			print("Auth type not supported for test")
			sys.exit(1)
		elif type == "E":
			print("ESP type not supported for test")
			sys.exit(1)
		else:
			print("Unknown EH type character")
			sys.exit(1)

		hdr = pair[0]/hdr
		len += pair[1]

		if (pair[2] == False):
			outcome = False

	return (hdr, len, next_hdr, outcome)
