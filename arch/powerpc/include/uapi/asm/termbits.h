/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
#ifndef _ASM_POWERPC_TERMBITS_H
#define _ASM_POWERPC_TERMBITS_H

/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <asm-generic/termbits-common.h>

typedef unsigned int	tcflag_t;

/*
 * termios type and macro definitions.  Be careful about adding stuff
 * to this file since it's used in GNU libc and there are strict rules
 * concerning namespace pollution.
 */

#define NCCS 19
struct termios {
	tcflag_t c_iflag;		/* input mode flags */
	tcflag_t c_oflag;		/* output mode flags */
	tcflag_t c_cflag;		/* control mode flags */
	tcflag_t c_lflag;		/* local mode flags */
	cc_t c_cc[NCCS];		/* control characters */
	cc_t c_line;			/* line discipline (== c_cc[19]) */
	speed_t c_ispeed;		/* input speed */
	speed_t c_ospeed;		/* output speed */
};

/* For PowerPC the termios and ktermios are the same */

struct ktermios {
	tcflag_t c_iflag;		/* input mode flags */
	tcflag_t c_oflag;		/* output mode flags */
	tcflag_t c_cflag;		/* control mode flags */
	tcflag_t c_lflag;		/* local mode flags */
	cc_t c_cc[NCCS];		/* control characters */
	cc_t c_line;			/* line discipline (== c_cc[19]) */
	speed_t c_ispeed;		/* input speed */
	speed_t c_ospeed;		/* output speed */
};

/* c_cc characters */
#define VINTR 	         0
#define VQUIT 	         1
#define VERASE 	         2
#define VKILL	         3
#define VEOF	         4
#define VMIN	         5
#define VEOL	         6
#define VTIME	         7
#define VEOL2	         8
#define VSWTC	         9
#define VWERASE 	10
#define VREPRINT	11
#define VSUSP 		12
#define VSTART		13
#define VSTOP		14
#define VLNEXT		15
#define VDISCARD	16

/* c_iflag bits */
#define IXON	0x0200U
#define IXOFF	0x0400U
#define IUCLC	0x1000U
#define IMAXBEL	0x2000U
#define IUTF8	0x4000U

/* c_oflag bits */
#define ONLCR	0x00002U
#define OLCUC	0x00004U
#define NLDLY	0x00300U
#define   NL0	0x00000U
#define   NL1	0x00100U
#define   NL2	0x00200U
#define   NL3	0x00300U
#define TABDLY	0x00c00U
#define   TAB0	0x00000U
#define   TAB1	0x00400U
#define   TAB2	0x00800U
#define   TAB3	0x00c00U
#define   XTABS	0x00c00U	/* required by POSIX to == TAB3 */
#define CRDLY	0x03000U
#define   CR0	0x00000U
#define   CR1	0x01000U
#define   CR2	0x02000U
#define   CR3	0x03000U
#define FFDLY	0x04000U
#define   FF0	0x00000U
#define   FF1	0x04000U
#define BSDLY	0x08000U
#define   BS0	0x00000U
#define   BS1	0x08000U
#define VTDLY	0x10000U
#define   VT0	0x00000U
#define   VT1	0x10000U

/* c_cflag bit meaning */
#define CBAUD		0x000000ffU
#define CBAUDEX		0x00000000U
#define BOTHER		0x0000001fU
#define    B57600	0x00000010U
#define   B115200	0x00000011U
#define   B230400	0x00000012U
#define   B460800	0x00000013U
#define   B500000	0x00000014U
#define   B576000	0x00000015U
#define   B921600	0x00000016U
#define  B1000000	0x00000017U
#define  B1152000	0x00000018U
#define  B1500000	0x00000019U
#define  B2000000	0x0000001aU
#define  B2500000	0x0000001bU
#define  B3000000	0x0000001cU
#define  B3500000	0x0000001dU
#define  B4000000	0x0000001eU
#define CSIZE		0x00000300U
#define   CS5		0x00000000U
#define   CS6		0x00000100U
#define   CS7		0x00000200U
#define   CS8		0x00000300U
#define CSTOPB		0x00000400U
#define CREAD		0x00000800U
#define PARENB		0x00001000U
#define PARODD		0x00002000U
#define HUPCL		0x00004000U
#define CLOCAL		0x00008000U
#define CIBAUD		0x00ff0000U

/* c_lflag bits */
#define ISIG	0x00000080U
#define ICANON	0x00000100U
#define XCASE	0x00004000U
#define ECHO	0x00000008U
#define ECHOE	0x00000002U
#define ECHOK	0x00000004U
#define ECHONL	0x00000010U
#define NOFLSH	0x80000000U
#define TOSTOP	0x00400000U
#define ECHOCTL	0x00000040U
#define ECHOPRT	0x00000020U
#define ECHOKE	0x00000001U
#define FLUSHO	0x00800000U
#define PENDIN	0x20000000U
#define IEXTEN	0x00000400U
#define EXTPROC	0x10000000U

/* Values for the OPTIONAL_ACTIONS argument to `tcsetattr'.  */
#define	TCSANOW		0
#define	TCSADRAIN	1
#define	TCSAFLUSH	2

#endif	/* _ASM_POWERPC_TERMBITS_H */
