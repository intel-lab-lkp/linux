# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_CPU_SUP_AMD)		+= core.o lbr.o
obj-$(CONFIG_PERF_EVENTS_AMD_BRS)	+= brs.o
obj-$(CONFIG_PERF_EVENTS_AMD_POWER)	+= power.o
obj-$(CONFIG_X86_LOCAL_APIC)		+= ibs.o
obj-$(CONFIG_PERF_EVENTS_UNCORE_COMMON) += uncore-common.o
uncore-common-objs := uncore_common.o
obj-$(CONFIG_PERF_EVENTS_AMD_UNCORE)	+= amd-uncore.o
amd-uncore-objs				:= uncore.o
obj-$(CONFIG_PERF_EVENTS_HYGON_UNCORE)	+= hygon-uncore.o
hygon-uncore-objs			:= hygon_uncore.o
ifdef CONFIG_AMD_IOMMU
obj-$(CONFIG_CPU_SUP_AMD)		+= iommu.o
endif
