/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CONSOLE_CMDLINE_H
#define _CONSOLE_CMDLINE_H

#define MAX_CMDLINECONSOLES 8

int console_opt_save(const char *str, const char *brl_opt);
int console_opt_add_preferred_console(const char *devname, const char *name,
				      const short idx, char *options,
				      char *brl_options);

struct console_cmdline
{
	char	name[16];			/* Name of the driver	    */
	int	index;				/* Minor dev. to use	    */
	char	devname[32];			/* DEVNAME:0.0 style device name */
	bool	user_specified;			/* Specified by command line vs. platform */
	char	*options;			/* Options for the driver   */
#ifdef CONFIG_A11Y_BRAILLE_CONSOLE
	char	*brl_options;			/* Options for braille driver */
#endif
};

#endif
