// SPDX-License-Identifier: GPL-2.0
/*
 *
 *	Based on work for the TITAN platform by Jeff Wiedemeier
 *	(Compaq Computer Corporation), Tsunami/Typhoon 21272 Chipset
 *	Hardware Reference Manual and AlphaServer ES40 Service Guide:
 *	EK-ES239-SV A01
 *
 *
 *	Author: Magnus Lindholm (linmag7@gmail.com)
 *
 *	Error handling code supporting TSUNAMI systems
 */

#include <linux/init.h>

#include <asm/io.h>
#include <asm/core_tsunami.h>
#include <asm/err_common.h>
#include <asm/err_ev6.h>
#include <asm/irq_regs.h>

#include "err_impl.h"
#include "proto.h"
#include "err_tsunami.h"



/*
 * System area for a clipper 680 environmental/system management mcheck
 */
struct el_CLIPPER_envdata_mcheck {
	u64 summary;    /* 0x00 */
	u64 c_dirx;     /* 0x08 */
	u64 smir;       /* 0x10 */
	u64 cpuir;      /* 0x18 */
	u64 psir;       /* 0x20 */
	u64 fault;      /* 0x28 */
	u64 sys_doors;  /* 0x30 */
	u64 temp_warn;  /* 0x38 */
	u64 fan_ctrl;   /* 0x40 */
	u64 code;       /* 0x48 */
	u64 reserved;   /* 0x50 */
};

static int
tsunami_parse_p_perror(int which, u64 perror, int print)
{
	int cmd;
	unsigned long addr;
	int status = MCHK_DISPOSITION_REPORT;

#ifdef CONFIG_VERBOSE_MCHECK
	static const char * const perror_cmd[] = {
		"Interrupt Acknowledge", "Special Cycle",
		"I/O Read",		"I/O Write",
		"Reserved",		"Reserved",
		"Memory Read",		"Memory Write",
		"Reserved",		"Reserved",
		"Configuration Read",	"Configuration Write",
		"Memory Read Multiple",	"Dual Address Cycle",
		"Memory Read Line",	"Memory Write and Invalidate"
	};
#endif /* CONFIG_VERBOSE_MCHECK */

#define TSUNAMI__PCHIP_PERROR__LOST	(1UL << 0)
#define TSUNAMI__PCHIP_PERROR__SERR	(1UL << 1)
#define TSUNAMI__PCHIP_PERROR__PERR	(1UL << 2)
#define TSUNAMI__PCHIP_PERROR__DCRTO	(1UL << 3)
#define TSUNAMI__PCHIP_PERROR__SGE	(1UL << 4)
#define TSUNAMI__PCHIP_PERROR__APE	(1UL << 5)
#define TSUNAMI__PCHIP_PERROR__TA	(1UL << 6)
#define TSUNAMI__PCHIP_PERROR__RDPE	(1UL << 7)
#define TSUNAMI__PCHIP_PERROR__NDS	(1UL << 8)
#define TSUNAMI__PCHIP_PERROR__UECC	(1UL << 10)
#define TSUNAMI__PCHIP_PERROR__CRE	(1UL << 11)

#define TSUNAMI__PCHIP_PERROR__ERRMASK	(TSUNAMI__PCHIP_PERROR__LOST |	\
					 TSUNAMI__PCHIP_PERROR__SERR |	\
					 TSUNAMI__PCHIP_PERROR__PERR |	\
					 TSUNAMI__PCHIP_PERROR__DCRTO |	\
					 TSUNAMI__PCHIP_PERROR__SGE |	\
					 TSUNAMI__PCHIP_PERROR__APE |	\
					 TSUNAMI__PCHIP_PERROR__TA |	\
					 TSUNAMI__PCHIP_PERROR__RDPE |	\
					 TSUNAMI__PCHIP_PERROR__NDS |	\
					 TSUNAMI__PCHIP_PERROR__UECC |	\
					 TSUNAMI__PCHIP_PERROR__CRE)
#define TSUNAMI__PCHIP_PERROR__DAC	(1UL << 16)
#define TSUNAMI__PCHIP_PERROR__MWIN	(1UL << 17)
#define TSUNAMI__PCHIP_PERROR__CMD__S	(52)
#define TSUNAMI__PCHIP_PERROR__CMD__M	(0x0f)
#define TSUNAMI__PCHIP_PERROR__SADDR__S	(19)
#define TSUNAMI__PCHIP_PERROR__SADDR__M	(0x7FFFFFFF80000ul)
#define TSUNAMI__PCHIP_PERROR__PADDR__S (18)
#define TSUNAMI__PCHIP_PERROR__PADDR__M (0x0FFFFFFFC0000ul)


	if (!(perror & TSUNAMI__PCHIP_PERROR__ERRMASK))
		return MCHK_DISPOSITION_UNKNOWN_ERROR;

	if (perror & (TSUNAMI__PCHIP_PERROR__UECC |
		TSUNAMI__PCHIP_PERROR__CRE))
		addr = EXTRACT(perror, TSUNAMI__PCHIP_PERROR__SADDR) >> 16;
	else
		addr = EXTRACT(perror, TSUNAMI__PCHIP_PERROR__PADDR) >> 16;

	cmd = EXTRACT(perror, TSUNAMI__PCHIP_PERROR__CMD);

	/*
	 * Initializing the BIOS on a video card on a bus without
	 * a south bridge (subtractive decode agent) can result in
	 * master aborts as the BIOS probes the capabilities of the
	 * card. XFree86 does such initialization. If the error
	 * is a master abort (No DevSel as PCI Master) and the command
	 * is an I/O read or write below the address where we start
	 * assigning PCI I/O spaces (SRM uses 0x1000), then mark the
	 * error as dismissable so starting XFree86 doesn't result
	 * in a series of uncorrectable errors being reported. Also
	 * dismiss master aborts to VGA frame buffer space
	 * (0xA0000 - 0xC0000) and legacy BIOS space (0xC0000 - 0x100000)
	 * for the same reason.
	 *
	 * Also mark the error dismissible if it looks like the right
	 * error but only the Lost bit is set. Since the BIOS initialization
	 * can cause multiple master aborts and the error interrupt can
	 * be handled on a different CPU than the BIOS code is run on,
	 * it is possible for a second master abort to occur between the
	 * time the PALcode reads PERROR and the time it writes PERROR
	 * to acknowledge the error. If this timing happens, a second
	 * error will be signalled after the first, and if no additional
	 * errors occur, will look like a Lost error with no additional
	 * errors on the same transaction as the previous error.
	 */

	/* unclear if this is correct on tsunami? maybe cmd==2 addr=0x80000
	 * and cmd==6 and addr<1d0000
	 */
	if (((perror & TSUNAMI__PCHIP_PERROR__NDS) ||
	     ((perror & TSUNAMI__PCHIP_PERROR__ERRMASK) ==
	      TSUNAMI__PCHIP_PERROR__LOST)) &&
	    ((((cmd & 0xE) == 2) && (addr <= 0x80000)) ||
	     (((cmd & 0xE) == 6) && (addr >= 0xA0000) && (addr < 0x100000)))) {
		status = MCHK_DISPOSITION_DISMISS;
	}

#ifdef CONFIG_VERBOSE_MCHECK
	if (!print)
		return status;

	printk("%s  PChip %d PERROR: %016llx\n",
	       err_print_prefix, which,
	       perror);
	if (perror & TSUNAMI__PCHIP_PERROR__NDS)
		printk("%s    No DEVSEL as PCI Master [Master Abort]\n",
		       err_print_prefix);
	if (perror & TSUNAMI__PCHIP_PERROR__TA)
		printk("%s    Target Abort\n", err_print_prefix);
	if (perror & TSUNAMI__PCHIP_PERROR__APE)
		printk("%s    Address Parity Error\n", err_print_prefix);
	if (perror & TSUNAMI__PCHIP_PERROR__SGE)
		printk("%s    Scatter-Gather Error, Invalid PTE\n",
		       err_print_prefix);
	if (perror & TSUNAMI__PCHIP_PERROR__DCRTO)
		printk("%s    Delayed-Completion Retry Timeout\n",
		       err_print_prefix);
	if (perror & TSUNAMI__PCHIP_PERROR__PERR)
		printk("%s    PERR Asserted\n", err_print_prefix);
	if (perror & TSUNAMI__PCHIP_PERROR__SERR)
		printk("%s    SERR Asserted\n", err_print_prefix);
	if (perror & TSUNAMI__PCHIP_PERROR__LOST)
		printk("%s    Lost Error\n", err_print_prefix);
	printk("%s      Command: 0x%x - %s\n"
		 "      Address: 0x%lx\n",
	       err_print_prefix,
	       cmd, perror_cmd[cmd],
	       addr);
	if (perror & TSUNAMI__PCHIP_PERROR__DAC)
		printk("%s      Dual Address Cycle\n", err_print_prefix);
	if (perror & TSUNAMI__PCHIP_PERROR__MWIN)
		printk("%s      Hit in Monster Window\n", err_print_prefix);
#endif /* CONFIG_VERBOSE_MCHECK */

	return status;
}

static int
tsunami_parse_c_misc(u64 c_misc, int print)
{
#ifdef CONFIG_VERBOSE_MCHECK
	char *src;
	int nxs = 0;
#endif
	int status = MCHK_DISPOSITION_REPORT;

#define TSUNAMI__CCHIP_MISC__NXM          (1UL << 28)
#define TSUNAMI__CCHIP_MISC__NXS__S       (29)
#define TSUNAMI__CCHIP_MISC__NXS__M       (0x7)

	if (!(c_misc & TSUNAMI__CCHIP_MISC__NXM))
		return MCHK_DISPOSITION_UNKNOWN_ERROR;

#ifdef CONFIG_VERBOSE_MCHECK
	if (!print)
		return status;

	nxs = EXTRACT(c_misc, TSUNAMI__CCHIP_MISC__NXS);
	switch (nxs) {
	case 0: /* CPU 0 */
	case 1: /* CPU 1 */
	case 2: /* CPU 2 */
	case 3: /* CPU 3 */
		src = "CPU";
		/* num is already the CPU number */
		break;
	case 4: /* Pchip 0 */
	case 5: /* Pchip 1 */
		src = "Pchip";
		nxs -= 4;
		break;
	default:/* reserved */
		src = "Unknown, NXS =";
		/* leave num untouched */
		break;
	}
	printk("%s    Non-existent memory access from: %s %d\n",
	       err_print_prefix, src, nxs);
#endif /* CONFIG_VERBOSE_MCHECK */

	return status;
}



static int
tsunami_parse_p_chip(int which, u64 perror, int print)
{
	int status = MCHK_DISPOSITION_UNKNOWN_ERROR;

	status |= tsunami_parse_p_perror(which, perror, print);
	return status;
}



int
tsunami_process_logout_frame(struct el_common *mchk_header, int print)
{
	struct el_TSUNAMI_sysdata_mcheck *tmchk =
		(struct el_TSUNAMI_sysdata_mcheck *)
		((unsigned long)mchk_header + mchk_header->sys_offset);
	int status = MCHK_DISPOSITION_UNKNOWN_ERROR;

	status |= tsunami_parse_c_misc(tmchk->misc, print);
	status |= tsunami_parse_p_chip(0, tmchk->p0_perror,
				     print);
	status |= tsunami_parse_p_chip(1, tmchk->p1_perror,
				     print);

	return status;
}

static inline void
tsunami_pci_clr_err_1(tsunami_pchip *pchip)
{
	pchip->perror.csr;
	pchip->perror.csr = 0x040;
	mb(); /* Is a memory barrier required* */
	pchip->perror.csr;
}

static inline void
tsunami_pci_clr_err(void)
{
	tsunami_pci_clr_err_1(TSUNAMI_pchip0);

	/* TSUNAMI and TYPHOON can have 2, but might only have 1 (DS10) */
	if (TSUNAMI_cchip->csc.csr & 1L<<14)
		tsunami_pci_clr_err_1(TSUNAMI_pchip1);
}

void
tsunami_machine_check(unsigned long vector, unsigned long la_ptr)
{
	struct el_common *mchk_header = (struct el_common *)la_ptr;
	char *reason;
	int status = 0;
	char *saved_err_prefix = err_print_prefix;
	/*
	 * Mask of Tsunami interrupt sources which are reported
	 * as machine checks.
	 *
	 * 63 - CChip Error
	 * 62 - PChip 0 H_Error
	 * 61 - PChip 1 H_Error
	 * 60 - PChip 0 C_Error
	 * 59 - PChip 1 C_Error
	 */
#define TSUNAMI_MCHECK_INTERRUPT_MASK	0xF800000000000000UL

	/*
	 * Sync the processor
	 */
	mb(); /* Double memory marriers */
	mb(); /* Special magic? */
	draina();
	/*
	 * Only handle system errors here
	 */
	switch (mchk_header->code) {
	/* Machine check reasons. Defined according to PALcode sources. */

	case 0x80:
		reason = "tag parity error";
		break;
	case 0x82:
		reason = "tag control parity error";
		break;
	case 0x84:
		reason = "generic hard error";
		break;
	case 0x86:
		reason = "correctable ECC error";
		break;
	case 0x88:
		reason = "uncorrectable ECC error";
		break;
	case 0x8A:
		reason = "OS-specific PAL bugcheck";
		break;
	case 0x90:
		reason = "callsys in kernel mode";
		break;
	case 0x96:
		reason = "i-cache read retryable error";
		break;
	case 0x98:
		reason = "processor detected hard error";
		break;

	/* System specific (these are for Alcor, at least): */
	case 0x202:
		reason = "system detected hard error";
		break;
	case 0x203:
		reason = "system detected uncorrectable ECC error";
		break;
	case 0x204:
		reason = "SIO SERR occurred on PCI bus";
		break;
	case 0x205:
		reason = "parity error detected by core logic";
		break;
	case 0x206:
		reason = "System environment error";
		break;
	case 0x207:
		reason = "non-existent memory error";
		break;
	case 0x208:
		reason = "MCHK_K_DCSR";
		break;
	case 0x209:
		reason = "PCI SERR detected";
		break;
	case 0x20b:
		reason = "PCI data parity error detected";
		break;
	case 0x20d:
		reason = "PCI address parity error detected";
		break;
	case 0x20f:
		reason = "PCI master abort error";
		break;
	case 0x211:
		reason = "PCI target abort error";
		break;
	case 0x213:
		reason = "scatter/gather PTE invalid error";
		break;
	case 0x215:
		reason = "flash ROM write error";
		break;
	case 0x217:
		reason = "IOA timeout detected";
		break;
	case 0x219:
		reason = "IOCHK#, EISA add-in board parity or other error";
		break;
	case 0x21b:
		reason = "EISA fail-safe timer timeout";
		break;
	case 0x21d:
		reason = "EISA bus time-out";
		break;
	case 0x21f:
		reason = "EISA software generated NMI";
		break;
	case 0x221:
		reason = "unexpected ev5 IRQ[3] interrupt";
		break;
	default:
		reason = "Unknown macine check code";
		break;
	}

	/*
	 * It's a system error, handle it here
	 *
	 * The PALcode has already cleared the error, so just parse it
	 */

	/*
	 * Parse the logout frame without printing first. If the only
	 * error(s)found are classified as "dismissable", then just
	 * dismiss them and don't print any message
	 */

	printk("%sSystem %s Error (Vector 0x%x) reported on CPU %d:\n",
		err_print_prefix,
		(vector == SCB_Q_SYSERR)?"Correctable":"Uncorrectable",
		(unsigned int)vector, (int)smp_processor_id());
	printk("Machine check error code is 0x%x (%s)",
		mchk_header->code, reason);
	status = clipper_process_logout_frame(mchk_header, 0);
	if (status != MCHK_DISPOSITION_DISMISS)	{
		err_print_prefix = KERN_CRIT;
		clipper_process_logout_frame(mchk_header, 1);
	}
	err_print_prefix = saved_err_prefix;

#ifdef CONFIG_VERBOSE_MCHECK
	if (alpha_verbose_mcheck)
		dik_show_regs(get_irq_regs(), NULL);
#endif /* CONFIG_VERBOSE_MCHECK */


	/* clear perror register, is this necessary or allready
	 * done by PALcode?
	 * */

	tsunami_pci_clr_err();

	/*
	 * Release the logout frame
	 */
	wrmces(0x7);
	mb(); /* Is a memory barrier required? */
}

/*
 * Subpacket Annotations
 */

static char *el_tsunami_pchip0_extended_annotation[] = {
"Subpacket Header",	"P0_PCTL",	"P0_PERRMASK",
"P0_WSBA0",		"P0_WSBA1",	"P0_WSBA2",	"P0_WSBA3",
"P0_WSM0",		"P0_WSM1",	"P0_WSM2",	"P0_WSM3",
"P0_TBA0",		"P0_TBA1",	"P0_TBA2",	"P0_TBA3"
};

static char *el_tsunami_pchip1_extended_annotation[] = {
"Subpacket Header",	"P1_PCTL",	"P1_PERRMASK",
"P1_WSBA0",		"P1_WSBA1",	"P1_WSBA2",	"P1_WSBA3",
"P1_WSM0",		"P1_WSM1",	"P1_WSM2",	"P1_WSM3",
"P1_TBA0",		"P1_TBA1",	"P1_TBA2",	"P1_TBA3"
};


#define EL_TYPE__REGATTA__TSUNAMI_PCHIP0_EXTENDED	6
#define EL_TYPE__REGATTA__TSUNAMI_PCHIP1_EXTENDED	7

static struct el_subpacket_annotation el_tsunami_annotations[] = {
	SUBPACKET_ANNOTATION(EL_CLASS__REGATTA_FAMILY,
			     EL_TYPE__REGATTA__TSUNAMI_PCHIP0_EXTENDED,
			     1,
			     "Tsunami PChip 0 Extended Frame",
			     el_tsunami_pchip0_extended_annotation),
	SUBPACKET_ANNOTATION(EL_CLASS__REGATTA_FAMILY,
			     EL_TYPE__REGATTA__TSUNAMI_PCHIP1_EXTENDED,
			     1,
			     "Tsunami PChip 1 Extended Frame",
			     el_tsunami_pchip1_extended_annotation),
	SUBPACKET_ANNOTATION(EL_CLASS__REGATTA_FAMILY,
			     EL_TYPE__TERMINATION__TERMINATION,
			     1,
			     "Termination Subpacket",
			     NULL)
};

static struct el_subpacket *
el_process_regatta_subpacket(struct el_subpacket *header)
{
	int status;

	if (header->class != EL_CLASS__REGATTA_FAMILY) {
		printk("%s  ** Unexpected header CLASS %d TYPE %d, aborting\n",
			err_print_prefix,
			header->class, header->type);
		return NULL;
	}

	switch (header->type) {
	case EL_TYPE__REGATTA__PROCESSOR_ERROR_FRAME:
	case EL_TYPE__REGATTA__SYSTEM_ERROR_FRAME:
	case EL_TYPE__REGATTA__ENVIRONMENTAL_FRAME:
	case EL_TYPE__REGATTA__PROCESSOR_DBL_ERROR_HALT:
	case EL_TYPE__REGATTA__SYSTEM_DBL_ERROR_HALT:
		printk("%s  ** Occurred on CPU %d:\n",
		       err_print_prefix,
		       (int)header->by_type.regatta_frame.cpuid);
		status = clipper_process_logout_frame((struct el_common *)
			header->by_type.regatta_frame.data_start, 0);
		if (status != MCHK_DISPOSITION_DISMISS)
			clipper_process_logout_frame((struct el_common *)
			header->by_type.regatta_frame.data_start, 1);
		break;
	default:
		printk("%s  ** REGATTA TYPE %d SUBPACKET\n",
		       err_print_prefix, header->type);
		el_annotate_subpacket(header);
		break;
	}


	return (struct el_subpacket *)((unsigned long)header + header->length);
}

static struct el_subpacket_handler tsunami_subpacket_handler =
	SUBPACKET_HANDLER_INIT(EL_CLASS__REGATTA_FAMILY,
				el_process_regatta_subpacket);



void __init
tsunami_register_error_handlers(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(el_tsunami_annotations); i++)
		cdl_register_subpacket_annotation(
		&el_tsunami_annotations[i]);

	cdl_register_subpacket_handler(&tsunami_subpacket_handler);

	ev6_register_error_handlers();
}

/*
 * Clipper
 */
static int
clipper_process_680_reg(char **msg, unsigned long reg, unsigned long mask
			, int length)
{
	int status = MCHK_DISPOSITION_UNKNOWN_ERROR;

	for (int i = 0; i < length; i++) {
		if (reg&mask&(1L<<i))
			printk("%s %s\n", err_print_prefix, msg[i]);
	}
	if (reg&mask)
		status = MCHK_DISPOSITION_REPORT;
	return status;
}

static int
clipper_process_680_frame(struct el_common *mchk_header, int print)
{
	int status = MCHK_DISPOSITION_UNKNOWN_ERROR;
	struct el_CLIPPER_envdata_mcheck *emchk =
		(struct el_CLIPPER_envdata_mcheck *)
		((unsigned long)mchk_header + mchk_header->sys_offset);
	if (mchk_header->code != 0x206)
		printk("Unknown machine check code=%x\n", mchk_header->code);


	/* Process erros in QW1SMIR */
	status |= clipper_process_680_reg(CLIPPER_EnvQW1SMIR,
				emchk->smir,
				CLIPPER_ENV_SMIR_MASK, 8);

	/* Process erros in QW2CPUIR */
	status |= clipper_process_680_reg(CLIPPER_EnvQW2CPUIR,
				emchk->cpuir,
				CLIPPER_ENV_CPUIR_MASK, 8);

	/* Process errors in QW3PSIR */
	status |= clipper_process_680_reg(CLIPPER_EnvQW3PSIR,
				emchk->psir,
				CLIPPER_ENV_PSIR_ERR_MASK, 8);

	/* Process enables PSU in QW3PSIR */
	status |= clipper_process_680_reg(CLIPPER_EnvQW3PSIR, emchk->psir,
				CLIPPER_ENV_PSIR_ENA_MASK, 8);

	/* Process errors in QW4LM78ISR */
	status |= clipper_process_680_reg(CLIPPER_EnvQW4LM78ISR,
				emchk->fault,
				CLIPPER_ENV_LM78ISR_MASK, 48);

	/* Process errors in QW5DOORS */
	status |= clipper_process_680_reg(CLIPPER_EnvQW5DOOR,
				emchk->sys_doors,
				CLIPPER_ENV_DOORS_MASK, 8);

	/* Process errors in QW6TEMP */
	status |= clipper_process_680_reg(CLIPPER_EnvQW6TEMP,
				emchk->temp_warn,
				CLIPPER_ENV_TEMP_MASK, 8);

	/* Process erros in QW7FAN */
	status |= clipper_process_680_reg(CLIPPER_EnvQW7FAN,
				emchk->temp_warn,
				CLIPPER_ENV_FAN_MASK, 12);

	/* Process erros in QW8POWER */
	status |= clipper_process_680_reg(CLIPPER_EnvQW8POWER,
				emchk->code,
				CLIPPER_ENV_POWER_MASK, 24);

	return status;
}

int
clipper_process_logout_frame(struct el_common *mchk_header, int print)
{
	struct el_common_EV6_mcheck *ev6mchk =
		(struct el_common_EV6_mcheck *)mchk_header;
	int status = MCHK_DISPOSITION_UNKNOWN_ERROR;

	/*
	 * Machine check codes
	 */
#define TSUNAMI_MCHK__CORR_ECC			0x86	/* 630 */
#define TSUNAMI_MCHK__DC_TAG_PERR		0x9E	/* 630 */
#define TSUNAMI_MCHK__PAL_BUGCHECK		0x8E	/* 670 */
#define TSUNAMI_MCHK__OS_BUGCHECK		0x90	/* 670 */
#define TSUNAMI_MCHK__PROC_HRD_ERR		0x98	/* 670 */
#define TSUNAMI_MCHK__ISTREAM_CMOV_PRX		0xA0	/* 670 */
#define TSUNAMI_MCHK__ISTREAM_CMOV_FLT		0xA2	/* 670 */
#define TSUNAMI_MCHK__SYS_HRD_ERR		0x202	/* 660 */
#define TSUNAMI_MCHK__SYS_CORR_ERR		0x204	/* 620 */
#define TSUNAMI_MCHK__SYS_ENVIRON		0x206	/* 680 */

	switch (ev6mchk->MCHK_Code) {
	/*
	 * Vector 630 - Processor, Correctable
	 */
	case TSUNAMI_MCHK__CORR_ECC:
	case TSUNAMI_MCHK__DC_TAG_PERR:
	/*
	 * Fall through to vector 670 for processing...
	 */

	/*
	 * Vector 670 - Processor, Uncorrectable
	 */
	case TSUNAMI_MCHK__PAL_BUGCHECK:
	case TSUNAMI_MCHK__OS_BUGCHECK:
	case TSUNAMI_MCHK__PROC_HRD_ERR:
	case TSUNAMI_MCHK__ISTREAM_CMOV_PRX:
	case TSUNAMI_MCHK__ISTREAM_CMOV_FLT:
		status = ev6_process_logout_frame(mchk_header, 0);
		if (status != MCHK_DISPOSITION_UNKNOWN_ERROR)
			status = ev6_process_logout_frame(mchk_header,
				print);
		break;

	/*
	 * Vector 620 - System, Correctable
	 */
	case TSUNAMI_MCHK__SYS_CORR_ERR:
		/*
		 * Fall through to vector 660 for processing...
		 */
	/*
	 * Vector 660 - System, Uncorrectable
	 */
	case TSUNAMI_MCHK__SYS_HRD_ERR:
		status |= tsunami_process_logout_frame(mchk_header,
			print);
		break;

	/*
	 * Vector 680 - System, Environmental
	 */
	case TSUNAMI_MCHK__SYS_ENVIRON:	/* System, Environmental */
		status |= clipper_process_680_frame(mchk_header,
			print);
		break;

	/*
	 * Unknown
	 */
	default:
		status |= MCHK_DISPOSITION_REPORT;
		if (print) {
			printk("%s** Unknown Error, frame follows\n",
				err_print_prefix);
			mchk_dump_logout_frame(mchk_header);
		}

	}

	return status;
}
