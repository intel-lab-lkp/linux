#
# Makefile for the Linux GeoNetworking layer.
#

obj-$(CONFIG_GN) += gn.o
gn-y			:= gn_prot.o gn_routing.o gn_netlink.o
gn-$(CONFIG_SYSCTL)	+= sysctl_net_gn.o
