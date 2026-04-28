// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <asm/bootinfo.h>
#include <asm/cpu.h>
#include <asm/cpu-features.h>
#include <asm/idle.h>
#include <asm/processor.h>
#include <asm/time.h>
#include <asm/cpufeature.h>

#define KERNEL_HWCAP(x)  const_ilog2(HWCAP_LOONGARCH_ ## x)
static const char *const hwcap_str[] = {
	[KERNEL_HWCAP(CPUCFG)]		= "cpucfg",
	[KERNEL_HWCAP(LAM)]		= "lam",
	[KERNEL_HWCAP(LAM_BH)]		= "lam_bh",
	[KERNEL_HWCAP(UAL)]		= "ual",
	[KERNEL_HWCAP(FPU)]		= "fpu",
	[KERNEL_HWCAP(LSX)]		= "lsx",
	[KERNEL_HWCAP(LASX)]		= "lasx",
	[KERNEL_HWCAP(CRC32)]		= "crc32",
	[KERNEL_HWCAP(COMPLEX)]		= "complex",
	[KERNEL_HWCAP(CRYPTO)]		= "crypto",
	[KERNEL_HWCAP(LVZ)]		= "lvz",
	[KERNEL_HWCAP(LBT_X86)]		= "lbt_x86",
	[KERNEL_HWCAP(LBT_ARM)]		= "lbt_arm",
	[KERNEL_HWCAP(LBT_MIPS)]	= "lbt_mips",
	[KERNEL_HWCAP(PTW)]		= "ptw",
	[KERNEL_HWCAP(LSPW)]		= "lspw",
	[KERNEL_HWCAP(SCQ)]		= "scq",
};

static int show_cpuinfo(struct seq_file *m, void *v)
{
	unsigned int i;
	unsigned long n = (unsigned long) v - 1;
	unsigned int isa = cpu_data[n].isa_level;
	unsigned int prid = cpu_data[n].processor_id;
	unsigned int version = cpu_data[n].processor_id & 0xff;
	unsigned int fp_version = cpu_data[n].fpu_vers;
	u64 freq = cpu_clock_freq, bogomips = lpj_fine * cpu_clock_freq;

#ifdef CONFIG_SMP
	if (!cpu_online(n))
		return 0;
#endif
	do_div(freq, 10000);
	do_div(bogomips, const_clock_freq * (5000/HZ));

	/*
	 * For the first processor also print the system type
	 */
	if (n == 0)
		seq_printf(m, "system type\t\t: %s\n\n", get_system_type());

	seq_printf(m, "processor\t\t: %ld\n", n);
	seq_printf(m, "package\t\t\t: %d\n", cpu_data[n].package);
	seq_printf(m, "core\t\t\t: %d\n", cpu_data[n].core);
	seq_printf(m, "global_id\t\t: %d\n", cpu_data[n].global_id);
	seq_printf(m, "CPU Family\t\t: %s\n", __cpu_family[n]);
	seq_printf(m, "Model Name\t\t: %s\n", __cpu_full_name[n]);
	seq_printf(m, "PRID\t\t\t: %s (%08x)\n", id_to_core_name(prid), prid);
	seq_printf(m, "CPU Revision\t\t: 0x%02x\n", version);
	seq_printf(m, "FPU Revision\t\t: 0x%02x\n", fp_version);
	seq_printf(m, "CPU MHz\t\t\t: %u.%02u\n", (u32)freq / 100, (u32)freq % 100);
	seq_printf(m, "BogoMIPS\t\t: %u.%02u\n", (u32)bogomips / 100, (u32)bogomips % 100);
	seq_printf(m, "TLB Entries\t\t: %d\n", cpu_data[n].tlbsize);
	seq_printf(m, "Address Sizes\t\t: %d bits physical, %d bits virtual\n",
		      cpu_pabits + 1, cpu_vabits + 1);

	seq_puts(m, "ISA\t\t\t:");
	if (isa & LOONGARCH_CPU_ISA_LA32R)
		seq_puts(m, " loongarch32r");
	if (isa & LOONGARCH_CPU_ISA_LA32S)
		seq_puts(m, " loongarch32s");
	if (isa & LOONGARCH_CPU_ISA_LA64)
		seq_puts(m, " loongarch64");
	seq_puts(m, "\n");

	seq_puts(m, "Features\t\t:");
	for (i = 0; i < ARRAY_SIZE(hwcap_str); i++)
		if (elf_hwcap & (1 << i))
			seq_printf(m, " %s", hwcap_str[i]);
	seq_puts(m, "\n");

	seq_printf(m, "Hardware Watchpoint\t: %s", str_yes_no(cpu_has_watch));
	if (cpu_has_watch) {
		seq_printf(m, ", iwatch count: %d, dwatch count: %d",
		      cpu_data[n].watch_ireg_count, cpu_data[n].watch_dreg_count);
	}

	seq_puts(m, "\n\n");

	return 0;
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	unsigned long i = *pos;

	return i < nr_cpu_ids ? (void *)(i + 1) : NULL;
}

static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return c_start(m, pos);
}

static void c_stop(struct seq_file *m, void *v)
{
}

const struct seq_operations cpuinfo_op = {
	.start	= c_start,
	.next	= c_next,
	.stop	= c_stop,
	.show	= show_cpuinfo,
};
