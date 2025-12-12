// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 - ARM Ltd
 */

#include <hyp/adjust_pc.h>

#include <linux/compiler.h>
#include <linux/irqchip/arm-gic-v5.h>
#include <linux/kvm_host.h>

#include <asm/kvm_emulate.h>
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>

void __vgic_v5_save_vmcr_aprs(struct vgic_v5_cpu_if *cpu_if)
{
	cpu_if->vgic_vmcr = read_sysreg_s(SYS_ICH_VMCR_EL2);
	cpu_if->vgic_apr = read_sysreg_s(SYS_ICH_APR_EL2);
}

static void  __vgic_v5_compat_mode_disable(void)
{
	sysreg_clear_set_s(SYS_ICH_VCTLR_EL2, ICH_VCTLR_EL2_V3, 0);
	isb();
}

void __vgic_v5_restore_vmcr_aprs(struct vgic_v5_cpu_if *cpu_if)
{
	__vgic_v5_compat_mode_disable();

	write_sysreg_s(cpu_if->vgic_vmcr, SYS_ICH_VMCR_EL2);
	write_sysreg_s(cpu_if->vgic_apr, SYS_ICH_APR_EL2);
}

void __vgic_v5_save_ppi_state(struct vgic_v5_cpu_if *cpu_if)
{
	cpu_if->vgic_ppi_activer_exit[0] =
		read_sysreg_s(SYS_ICH_PPI_ACTIVER0_EL2);
	cpu_if->vgic_ppi_activer_exit[1] =
		read_sysreg_s(SYS_ICH_PPI_ACTIVER1_EL2);

	cpu_if->vgic_ich_ppi_enabler_exit[0] =
		read_sysreg_s(SYS_ICH_PPI_ENABLER0_EL2);
	cpu_if->vgic_ich_ppi_enabler_exit[1] =
		read_sysreg_s(SYS_ICH_PPI_ENABLER1_EL2);

	cpu_if->vgic_ppi_pendr_exit[0] = read_sysreg_s(SYS_ICH_PPI_PENDR0_EL2);
	cpu_if->vgic_ppi_pendr_exit[1] = read_sysreg_s(SYS_ICH_PPI_PENDR1_EL2);

	cpu_if->vgic_ppi_priorityr[0] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR0_EL2);
	cpu_if->vgic_ppi_priorityr[1] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR1_EL2);
	cpu_if->vgic_ppi_priorityr[2] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR2_EL2);
	cpu_if->vgic_ppi_priorityr[3] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR3_EL2);
	cpu_if->vgic_ppi_priorityr[4] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR4_EL2);
	cpu_if->vgic_ppi_priorityr[5] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR5_EL2);
	cpu_if->vgic_ppi_priorityr[6] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR6_EL2);
	cpu_if->vgic_ppi_priorityr[7] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR7_EL2);
	cpu_if->vgic_ppi_priorityr[8] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR8_EL2);
	cpu_if->vgic_ppi_priorityr[9] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR9_EL2);
	cpu_if->vgic_ppi_priorityr[10] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR10_EL2);
	cpu_if->vgic_ppi_priorityr[11] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR11_EL2);
	cpu_if->vgic_ppi_priorityr[12] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR12_EL2);
	cpu_if->vgic_ppi_priorityr[13] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR13_EL2);
	cpu_if->vgic_ppi_priorityr[14] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR14_EL2);
	cpu_if->vgic_ppi_priorityr[15] =
		read_sysreg_s(SYS_ICH_PPI_PRIORITYR15_EL2);

	/* Now that we are done, disable DVI */
	write_sysreg_s(0, SYS_ICH_PPI_DVIR0_EL2);
	write_sysreg_s(0, SYS_ICH_PPI_DVIR1_EL2);
}

void __vgic_v5_restore_ppi_state(struct vgic_v5_cpu_if *cpu_if)
{
	 /* Now enable DVI so that the guest's interrupt config takes over */
	 write_sysreg_s(cpu_if->vgic_ppi_dvir[0], SYS_ICH_PPI_DVIR0_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_dvir[1], SYS_ICH_PPI_DVIR1_EL2);

	 write_sysreg_s(cpu_if->vgic_ppi_activer_entry[0],
			SYS_ICH_PPI_ACTIVER0_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_activer_entry[1],
			SYS_ICH_PPI_ACTIVER1_EL2);

	 write_sysreg_s(cpu_if->vgic_ich_ppi_enabler_entry[0],
			SYS_ICH_PPI_ENABLER0_EL2);
	 write_sysreg_s(cpu_if->vgic_ich_ppi_enabler_entry[1],
			SYS_ICH_PPI_ENABLER1_EL2);

	 /* Update the pending state of the NON-DVI'd PPIs, only */
	 write_sysreg_s(cpu_if->vgic_ppi_pendr_entry[0] &
				~cpu_if->vgic_ppi_dvir[0],
			SYS_ICH_PPI_PENDR0_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_pendr_entry[1] &
				~cpu_if->vgic_ppi_dvir[1],
			SYS_ICH_PPI_PENDR1_EL2);

	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[0],
			SYS_ICH_PPI_PRIORITYR0_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[1],
			SYS_ICH_PPI_PRIORITYR1_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[2],
			SYS_ICH_PPI_PRIORITYR2_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[3],
			SYS_ICH_PPI_PRIORITYR3_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[4],
			SYS_ICH_PPI_PRIORITYR4_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[5],
			SYS_ICH_PPI_PRIORITYR5_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[6],
			SYS_ICH_PPI_PRIORITYR6_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[7],
			SYS_ICH_PPI_PRIORITYR7_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[8],
			SYS_ICH_PPI_PRIORITYR8_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[9],
			SYS_ICH_PPI_PRIORITYR9_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[10],
			SYS_ICH_PPI_PRIORITYR10_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[11],
			SYS_ICH_PPI_PRIORITYR11_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[12],
			SYS_ICH_PPI_PRIORITYR12_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[13],
			SYS_ICH_PPI_PRIORITYR13_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[14],
			SYS_ICH_PPI_PRIORITYR14_EL2);
	 write_sysreg_s(cpu_if->vgic_ppi_priorityr[15],
			SYS_ICH_PPI_PRIORITYR15_EL2);
}

void __vgic_v5_save_icsr(struct vgic_v5_cpu_if *cpu_if)
{
	cpu_if->vgic_icsr = read_sysreg_s(SYS_ICC_ICSR_EL1);
}

void __vgic_v5_restore_icsr(struct vgic_v5_cpu_if *cpu_if)
{
	write_sysreg_s(cpu_if->vgic_icsr, SYS_ICC_ICSR_EL1);
}
