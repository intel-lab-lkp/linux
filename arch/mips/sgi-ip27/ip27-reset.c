/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Reset an IP27, or an IP35-family (Bedrock hub) machine.
 *
 * On Bedrock hubs restart and halt ask the L1 system controller to reset
 * the machine and fall back to an NI port reset if it has not acted
 * within five seconds; poweroff asks the L1 to cut power.
 *
 * Copyright (C) 1997, 1998, 1999, 2000, 06 by Ralf Baechle
 * Copyright (C) 1999, 2000 Silicon Graphics, Inc.
 */
#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/smp.h>
#include <linux/mmzone.h>
#include <linux/nodemask.h>
#include <linux/pm.h>

#include <asm/io.h>
#include <asm/irq.h>
#include <asm/reboot.h>
#include <asm/sgialib.h>
#include <asm/sn/addrs.h>
#include <asm/sn/agent.h>
#include <asm/sn/arch.h>
#include <asm/sn/gda.h>
#include <asm/sn/l1.h>

#include "ip27-common.h"

void machine_restart(char *command) __noreturn;
void machine_halt(void) __noreturn;
void machine_power_off(void) __noreturn;

#define noreturn while(1);				/* Silence gcc.	 */

static void ip35_reset(void)
{
	l1_exec_command("rst");
	mdelay(5000);
	LOCAL_HUB_S(NI_RESET_ENABLE_IP35, NI_RESET_OK_IP35);
	LOCAL_HUB_S(NI_PORT_RESET_IP35,
		    NI_RESET_ACTION_IP35 | NI_RESET_LOCAL_IP35);
}

/* XXX How to pass the reboot command to the firmware??? */
static void ip27_machine_restart(char *command)
{
#if 0
	int i;
#endif

	printk("Reboot started from CPU %d\n", smp_processor_id());
#ifdef CONFIG_SMP
	smp_send_stop();
#endif
	if (system_is_ip35) {
		ip35_reset();
	} else {
#if 0
		for_each_online_node(i)
			REMOTE_HUB_S(i, PROMOP_REG, PROMOP_REBOOT);
#else
		LOCAL_HUB_S(NI_PORT_RESET, NPR_PORTRESET | NPR_LOCALRESET);
#endif
	}
	noreturn;
}

static void ip27_machine_halt(void)
{
	int i;

#ifdef CONFIG_SMP
	smp_send_stop();
#endif
	if (system_is_ip35) {
		ip35_reset();
	} else {
		for_each_online_node(i)
			REMOTE_HUB_S(i, PROMOP_REG, PROMOP_RESTART);
		LOCAL_HUB_S(NI_PORT_RESET, NPR_PORTRESET | NPR_LOCALRESET);
	}
	noreturn;
}

static void ip27_machine_power_off(void)
{
	if (system_is_ip35) {
#ifdef CONFIG_SMP
		smp_send_stop();
#endif
		l1_exec_command("pwr d");
		mdelay(5000);
		pr_info("Please power off the system manually.\n");
	}
	/* IP27 (Hub) has no software poweroff path. */
	noreturn;
}

void ip27_reboot_setup(void)
{
	_machine_restart = ip27_machine_restart;
	_machine_halt = ip27_machine_halt;
	pm_power_off = ip27_machine_power_off;
}
