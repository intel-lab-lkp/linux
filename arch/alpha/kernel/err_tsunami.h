/* SPDX-License-Identifier: GPL-2.0 */
/*
 *
 *	Author:	Magnus Lindholm linmag7@gmail.com
 *
 *	Contains declarations and macros to support Alpha error handling
 *      and error messages for the tsunami/typhoon based platforms
 */

static char *CLIPPER_EnvQW1SMIR[] = {
"System Power Supply state change detected",
"OCP or RMC halt detected",
"Sys_DC_Notok failure detected",
"",
"System temperature over 50 degrees C failure",
"PCI Bus #0 is in reset",
"PCI Bus #1 is in reset",
"System is being reset"
};

#define CLIPPER_ENV_QW1SMIR_POWER_MASK	1L
#define CLIPPER_EVN_QW1SMIR_RMC_MASK	(1L<<1)
#define CLIPPER_EVN_QW1SMIR_DC_MASK	(1L<<2)
#define CLIPPER_EVN_QW1SMIR_TEMP_MASK	(1L<<4)
#define CLIPPER_ENV_SMIR_MASK (CLIPPER_ENV_QW1SMIR_POWER_MASK |	\
				CLIPPER_EVN_QW1SMIR_RMC_MASK |	\
				CLIPPER_EVN_QW1SMIR_DC_MASK |	\
				CLIPPER_EVN_QW1SMIR_TEMP_MASK)
static char *CLIPPER_EnvQW2CPUIR[] = {
"CPU0 regulator is enabled",
"CPU1 regulator is enabled",
"CPU2 regulator is enabled",
"CPU3 regulator is enabled",
"CPU0 regulator or configuration sequence fail",
"CPU1 regulator or configuration sequence fail",
"CPU2 regulator or configuration sequence fail",
"CPU3 regulator or configuration sequence fail"
};

#define CLIPPER_ENV_QW2CPUIR_CPU0_ENA_MASK	1L
#define CLIPPER_ENV_QW2CPUIR_CPU1_ENA_MASK	(1L<<1)
#define CLIPPER_ENV_QW2CPUIR_CPU2_ENA_MASK	(1L<<2)
#define CLIPPER_ENV_QW2CPUIR_CPU3_ENA_MASK	(1L<<3)
#define CLIPPER_ENV_QW2CPUIR_CPU0_FAIL_MASK	(1L<<4)
#define CLIPPER_ENV_QW2CPUIR_CPU1_FAIL_MASK	(1L<<5)
#define CLIPPER_ENV_QW2CPUIR_CPU2_FAIL_MASK	(1L<<6)
#define CLIPPER_ENV_QW2CPUIR_CPU3_FAIL_MASK	(1L<<7)
#define CLIPPER_ENV_CPUIR_MASK	(CLIPPER_ENV_QW2CPUIR_CPU0_FAIL_MASK |	\
				CLIPPER_ENV_QW2CPUIR_CPU1_FAIL_MASK |	\
				CLIPPER_ENV_QW2CPUIR_CPU2_FAIL_MASK |	\
				CLIPPER_ENV_QW2CPUIR_CPU3_FAIL_MASK)


static char *CLIPPER_EnvQW3PSIR[] = {
"Power Supply 0 is enabled",
"Power Supply 1 is enabled",
"Power Supply 2 is enabled",
"",
"Power Supply 0 was enabled but failed",
"Power Supply 1 was enabled but failed",
"Power Supply 2 was enabled but failed"
};

#define CLIPPER_PSIR_PSU0_ENA_MASK	1L
#define CLIPPER_PSIR_PSU1_ENA_MASK	(1L<<1)
#define CLIPPER_PSIR_PSU2_ENA_MASK	(1L<<2)
#define CLIPPER_PSIR_PSU0_FAIL_MASK	(1L<<4)
#define CLIPPER_PSIR_PSU1_FAIL_MASK	(1L<<5)
#define CLIPPER_PSIR_PSU2_FAIL_MASK	(1L<<6)
#define CLIPPER_ENV_PSIR_ENA_MASK (CLIPPER_PSIR_PSU0_ENA_MASK	|	\
				CLIPPER_PSIR_PSU1_ENA_MASK	|	\
				CLIPPER_PSIR_PSU2_ENA_MASK)
#define CLIPPER_ENV_PSIR_ERR_MASK (CLIPPER_PSIR_PSU0_FAIL_MASK	|	\
				CLIPPER_PSIR_PSU1_FAIL_MASK	|	\
				CLIPPER_PSIR_PSU2_FAIL_MASK)

static char *CLIPPER_EnvQW4LM78ISR[] = {
"PS +3.3V out of tolerance",
"PS +5V out of tolerance",
"PS +12V out of tolerance",
"VTERM out of tolerance",
"Temperature zone 0 (PCI Backplane slots 1-3 area) over limit failure",
"LM75 CPU0-3 Temperature over limit failure",
"System Fan 1 failure",
"System Fan 2 failure",
"CTERM out of tolerance",
"",
"-12V out of tolerance",
"",
"",
"",
"",
"",
"CPU0_VCORE +2V out of tolerance",
"CPU0_VIO +1.5V out of tolerance",
"CPU1_VCORE +2V out of tolerance",
"CPU1_VIO +1.5V out of tolerance",
"Temperature zone 1 (PCI Backplane slots 7-10 area) over limit failure",
"",
"System Fan 4 failure",
"System Fan 5 failure",
"",
"",
"",
"",
"",
"",
"",
"",
"CPU2_VCORE +2V out of tolerance",
"CPU2_VIO +1.5V out of tolerance",
"CPU3_VCORE +2V out of tolerance",
"CPU3_VIO +1.5V out of tolerance",
"Temperature zone 2 (PCI Backplane slots 4-6 area) over limit failure",
"",
"System Fan 3 failure",
"System Fan 6 failure",
"",
"",
"Power supply 3.3V rail above high amperage warning",
"Power supply 5.0V rail above high amperage warning",
"Power supply 12V rail above high amperage warning",
"Power supply high temperature warning",
"Power supply AC input low limit warning",
"Power supply AC input high limit warning"
};

#define CLIPPER_ENV_LM78ISR_MASK	0xFCDF00DF05FFL

static char *CLIPPER_EnvQW5DOOR[] = {
"",
"Set = System CPU door is open",
"Set = System Fan door is open",
"Set = System PCI door is open",
"",
"Set = System CPU door is closed",
"Set = System Fan door is closed",
"Set = System PCI door is closed"
};

#define CLIPPER_ENV_DOORS_MASK	 0xEEL

static char *CLIPPER_EnvQW6TEMP[] = {
"CPU0 temperature warning fault has occurred",
"CPU1 temperature warning fault has occurred",
"CPU2 temperature warning fault has occurred",
"CPU3 temperature warning fault has occurred",
"System temperature zone 0 warning fault has occurred",
"System temperature zone 1 warning fault has occurred",
"System temperature zone 2 warning fault has occurred"
};

#define CLIPPER_ENV_TEMP_MASK	0xFFL

static char *CLIPPER_EnvQW7FAN[] = {
"System Fan 1 is not responding to RMC Commands",
"System Fan 2 is not responding to RMC Commands",
"System Fan 3 is not responding to RMC Commands",
"System Fan 4 is not responding to RMC Commands",
"System Fan 5 is not responding to RMC Commands",
"System Fan 6 is not responding to RMC Commands",
"",
"",
"CPU fans 5/6 at maximum speed",
"CPU fans 5/6 reduced speed from maximum",
"PCI fans 1-4 at maximum speed",
"PCI fans 1-4 reduced speed from maximum"
};

#define CLIPPER_ENV_FAN_MASK	0xF3FL

static char *CLIPPER_EnvQW8POWER[] = {
"Power Supply 0 AC input fail",
"Power Supply 1 AC input fail",
"Power Supply 2 AC input fail",
"",
"",
"",
"",
"",
"Power Supply 0 DC fail",
"Power Supply 1 DC fail",
"Power Supply 2 DC fail",
"Vterm fail",
"CPU0 Regulator fail",
"CPU1 Regulator fail",
"CPU2 Regulator fail",
"CPU3 Regulator fail",
"",
"No CPU in system motherboard CPU slot 0",
"Invalid CPU SROM voltage setting or checksum",
"TIG load initialization or sequence fail",
"Over temperature fail",
"CPU door open fail",
"System fan 5 (CPU backup fan) fail",
"Cterm fail"
};

#define CLIPPER_ENV_POWER_MASK	0xFEFF07L
