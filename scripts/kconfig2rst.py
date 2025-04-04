#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright 2025 Collabora Ltd

import sys
import re
import os

import argparse

BASE_PATH_DEFAULT = "Documentation/Config/"
CFG_LEN = 60
RE_indentation = r"^[ \t]*"
in_help_txt = False
help_txt = ""

# These configs appear more than once, thus we don't generate labels or xrefs to
# them to avoid duplicate label warnings from Sphinx
REPEATED_CONFIGS = [
    "32BIT",
    "4KSTACKS",
    "64BIT",
    "A",
    "ADVANCED_OPTIONS",
    "ALPHA_LEGACY_START_ADDRESS",
    "ARCH_AIROHA",
    "ARCH_ALPINE",
    "ARCH_BCM2835",
    "ARCH_BCM_IPROC",
    "ARCH_BRCMSTB",
    "ARCH_DEFAULT_CRASH_DUMP",
    "ARCH_FLATMEM_ENABLE",
    "ARCH_FORCE_MAX_ORDER",
    "ARCH_HAS_ADD_PAGES",
    "ARCH_HAS_CACHE_LINE_SIZE",
    "ARCH_HAS_GENERIC_CRASHKERNEL_RESERVATION",
    "ARCH_HAS_ILOG2_U32",
    "ARCH_HAS_ILOG2_U64",
    "ARCH_HIBERNATION_HEADER",
    "ARCH_HIBERNATION_POSSIBLE",
    "ARCH_HISI",
    "ARCH_MAY_HAVE_PC_FDC",
    "ARCH_MEMORY_PROBE",
    "ARCH_MMAP_RND_BITS_MAX",
    "ARCH_MMAP_RND_BITS_MIN",
    "ARCH_MMAP_RND_COMPAT_BITS_MAX",
    "ARCH_MMAP_RND_COMPAT_BITS_MIN",
    "ARCH_MTD_XIP",
    "ARCH_OMAP",
    "ARCH_PKEY_BITS",
    "ARCH_PROC_KCORE_TEXT",
    "ARCH_R9A07G043",
    "ARCH_RENESAS",
    "ARCH_ROCKCHIP",
    "ARCH_SELECT_MEMORY_MODEL",
    "ARCH_SELECTS_CRASH_DUMP",
    "ARCH_SELECTS_KEXEC_FILE",
    "ARCH_SPARSEMEM_DEFAULT",
    "ARCH_SPARSEMEM_ENABLE",
    "ARCH_SUNXI",
    "ARCH_SUPPORTS_CRASH_DUMP",
    "ARCH_SUPPORTS_CRASH_HOTPLUG",
    "ARCH_SUPPORTS_KEXEC",
    "ARCH_SUPPORTS_KEXEC_FILE",
    "ARCH_SUPPORTS_KEXEC_JUMP",
    "ARCH_SUPPORTS_KEXEC_PURGATORY",
    "ARCH_SUPPORTS_KEXEC_SIG",
    "ARCH_SUPPORTS_UPROBES",
    "ARCH_SUSPEND_POSSIBLE",
    "ARCH_UNIPHIER",
    "ARCH_VIRT",
    "AUDIT_ARCH",
    "B",
    "BCH_CONST_M",
    "BCH_CONST_T",
    "BUILTIN_DTB",
    "BUILTIN_DTB_NAME",
    "C",
    "CC_HAVE_STACKPROTECTOR_TLS",
    "CHOICE_B",
    "CHOICE_C",
    "CMDLINE",
    "CMDLINE_BOOL",
    "CMDLINE_EXTEND",
    "CMDLINE_FORCE",
    "CMDLINE_FROM_BOOTLOADER",
    "CMDLINE_OVERRIDE",
    "CMM",
    "COMPAT",
    "COMPAT_VDSO",
    "CORE",
    "CORE_BELL_A",
    "CORE_BELL_A_ADVANCED",
    "CPU_BIG_ENDIAN",
    "CPU_HAS_FPU",
    "CPU_HAS_PREFETCH",
    "CPU_LITTLE_ENDIAN",
    "CRYPTO_CHACHA20_NEON",
    "CRYPTO_JITTERENTROPY_MEMORY_BLOCKS",
    "CRYPTO_JITTERENTROPY_MEMORY_BLOCKSIZE",
    "CRYPTO_JITTERENTROPY_OSR",
    "CRYPTO_JITTERENTROPY_TESTINTERFACE",
    "CRYPTO_NHPOLY1305_NEON",
    "DEBUG_ENTRY",
    "DMA_NONCOHERENT",
    "DMI",
    "DRAM_BASE",
    "DUMMY",
    "DUMMY_CONSOLE",
    "EARLY_PRINTK",
    "EFI",
    "EFI_STUB",
    "FIT_IMAGE_FDT_EPM5",
    "FIX_EARLYCON_MEM",
    "FPU",
    "GENERIC_BUG",
    "GENERIC_BUG_RELATIVE_POINTERS",
    "GENERIC_CALIBRATE_DELAY",
    "GENERIC_CSUM",
    "GENERIC_HWEIGHT",
    "GENERIC_ISA_DMA",
    "GENERIC_LOCKBREAK",
    "HAS_IOMEM",
    "HAVE_SMP",
    "HAVE_TCM",
    "HEARTBEAT",
    "HIGHMEM",
    "HOTPLUG_CPU",
    "HW_PERF_EVENTS",
    "HZ",
    "HZ_100",
    "HZ_1000",
    "HZ_1024",
    "HZ_128",
    "HZ_250",
    "HZ_256",
    "ILLEGAL_POINTER_VALUE",
    "IRQSTACKS",
    "ISA",
    "ISA_DMA_API",
    "KASAN_SHADOW_OFFSET",
    "KERNEL_MODE_NEON",
    "KERNEL_START",
    "KERNEL_START_BOOL",
    "KUSER_HELPERS",
    "KVM",
    "KVM_GUEST",
    "L1_CACHE_SHIFT",
    "LEDS_EXPRESSWIRE",
    "LOCKDEP_SUPPORT",
    "LOWMEM_SIZE",
    "LOWMEM_SIZE_BOOL",
    "MACH_LOONGSON32",
    "MACH_LOONGSON64",
    "MACH_TX49XX",
    "MAGIC_SYSRQ",
    "MATH_EMULATION",
    "MCOUNT",
    "MMU",
    "NODES_SHIFT",
    "NO_IOPORT_MAP",
    "NR_CPUS",
    "NR_CPUS_DEFAULT",
    "NR_CPUS_RANGE_END",
    "NUMA",
    "PAGE_OFFSET",
    "PANIC_TIMEOUT",
    "PARAVIRT",
    "PARAVIRT_SPINLOCKS",
    "PARAVIRT_TIME_ACCOUNTING",
    "PFAULT",
    "PGTABLE_LEVELS",
    "PHYSICAL_ALIGN",
    "PHYSICAL_START",
    "PID_IN_CONTEXTIDR",
    "PM",
    "POWERPC64_CPU",
    "PRINT_STACK_DEPTH",
    "RANDOMIZE_BASE",
    "RANDOMIZE_BASE_MAX_OFFSET",
    "RELOCATABLE",
    "SBUS",
    "SCHED_CLUSTER",
    "SCHED_HRTICK",
    "SCHED_MC",
    "SCHED_OMIT_FRAME_POINTER",
    "SCHED_SMT",
    "SERIAL_CONSOLE",
    "SMP",
    "STACKPROTECTOR_PER_TASK",
    "STACKTRACE_SUPPORT",
    "SWAP_IO_SPACE",
    "SYS_SUPPORTS_APM_EMULATION",
    "SYS_SUPPORTS_NUMA",
    "SYS_SUPPORTS_SMP",
    "TASK_SIZE",
    "TASK_SIZE_BOOL",
    "TCP_CONG_CUBIC",
    "TIME_LOW_RES",
    "UNWINDER_FRAME_POINTER",
    "UNWINDER_GUESS",
    "UNWINDER_ORC",
    "USE_OF",
    "VMSPLIT_1G",
    "VMSPLIT_2G",
    "VMSPLIT_3G",
    "VMSPLIT_3G_OPT",
    "X",
    "X86_32",
    "X86_64",
    "XEN",
    "XEN_DOM0",
    "XIP_KERNEL",
    "XIP_PHYS_ADDR",
    "ARCH_BCM",
    "VIRTUALIZATION",
]


def print_title(title):
    heading = "=" * len(title)
    print(heading)
    print(title)
    print(heading)
    print()


parser = argparse.ArgumentParser(
    prog="kconfig2rst", description="Convert a Kconfig file into ReStructuredText"
)

parser.add_argument("kconfig", help="Path to input Kconfig file")
parser.add_argument(
    "--base-doc-path",
    default=BASE_PATH_DEFAULT,
    help="Base path of generated rST files for usage in 'source' links",
)
args = parser.parse_args()

print_title(args.kconfig)

line_accum = ""
continued_line = False

repeated_config_count = {}

with open(args.kconfig) as f:
    for il in f:
        # If line ends with \, accumulate it and handle full line
        if re.search(r"\\\n$", il):
            continued_line = True
            line_accum += il[:-2]  # accumulate without backslash and newline
            continue

        if continued_line:
            continued_line = False
            l = line_accum + il
            line_accum = ""
        else:
            l = il

        if in_help_txt:
            if l == "\n":
                help_txt += l
                continue
            if first_line_help_txt:
                help_txt_indentation = re.match(RE_indentation, l).group(0).expandtabs()
                first_line_help_txt = False
            # Consider any line with same or more indentation as part of help text
            if (
                help_txt_indentation
                in re.match(RE_indentation, l).group(0).expandtabs()
            ):
                help_txt += l
                continue
            else:
                in_help_txt = False
                print(help_txt)
                help_txt = ""
        else:
            l = re.sub(r"[*]", r"\*", l)  # Escape asterisks

        if re.match(r"^[ \t]*#.*", l):
            # Skip comments
            continue

        if re.match(r"^[ \t]*help", l):
            in_help_txt = True
            first_line_help_txt = True
            print("* help::\n")
            continue

        m = re.match("^[ \t]*(menu)?config (?P<cfgname>[A-Za-z0-9_]+)", l)
        if m:
            section_name = f"\nCONFIG_{m.group('cfgname')}"
            underline = f"\n{'='*CFG_LEN}\n"
            if m.group("cfgname") in REPEATED_CONFIGS:
                repeated_config_count[m.group("cfgname")] = (
                    repeated_config_count.get(m.group("cfgname"), 0) + 1
                )
                if repeated_config_count[m.group("cfgname")] > 1:
                    section_name += f"({repeated_config_count[m.group('cfgname')]})"
                print(section_name + underline)
            else:
                print(f"\n.. _CONFIG_{m.group('cfgname')}:\n\n" + section_name + underline)
            continue

        m = re.match(
            r"^[ \t]*(def_bool|def_tristate|depends on|select|range|visible if|imply|default|prompt|bool|tristate|string|hex|int|modules)( \"(.*)\")?(?P<expr> [^\"]*)?",
            l,
        )
        if m:
            expr = m.group('expr') if m.group('expr') else ''
            not_expr = l
            if expr:
                expr = re.sub(r'[A-Z0-9_]{2,}', rf" CONFIG_\g<0> ", expr)
                not_expr = l[:m.start('expr')]
            print("* " + not_expr.lstrip() + expr.rstrip())
            continue

        m = re.match(r'^[ \t]*source "(.*)"', l)
        if m:
            # Format Kconfig file paths as Documentation/... so they can be turned
            # into links by the automarkup plugin
            print(f"\nsource {args.base_doc_path + m.group(1)}.rst\n")
            continue

        m = re.match(r"[^ \t]*choice|endchoice|comment|menu|endmenu|if|endif", l)
        if m:
            print("\n" + l.strip() + "\n")
            continue

        print(l.strip())

if help_txt:
    print(help_txt)  # Flush any pending help text
