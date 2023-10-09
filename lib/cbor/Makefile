# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_CBOR) += cbor.o

cbor-y := \
	common.o \
	decoder.o \
	encoder.o \
	helper.o \
	parser.o \

cbor-$(CONFIG_CBOR_FLOAT) += ieee754.o
