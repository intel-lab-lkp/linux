/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1995, 96, 99, 2001, 06 Ralf Baechle
 * Copyright (C) 1999 Silicon Graphics, Inc.
 * Copyright (C) 2001 MIPS Technologies, Inc.
 */
#ifndef _ASM_TERMBITS_H
#define _ASM_TERMBITS_H

#include <asm-generic/termbits-common.h>

typedef unsigned int	tcflag_t;

/*
 * The ABI says nothing about NCC but seems to use NCCS as
 * replacement for it in struct termio
 */
#define NCCS	23
struct termios {
	tcflag_t c_iflag;		/* input mode flags */
	tcflag_t c_oflag;		/* output mode flags */
	tcflag_t c_cflag;		/* control mode flags */
	tcflag_t c_lflag;		/* local mode flags */
	cc_t c_line;			/* line discipline */
	cc_t c_cc[NCCS];		/* control characters */
};

struct termios2 {
	tcflag_t c_iflag;		/* input mode flags */
	tcflag_t c_oflag;		/* output mode flags */
	tcflag_t c_cflag;		/* control mode flags */
	tcflag_t c_lflag;		/* local mode flags */
	cc_t c_line;			/* line discipline */
	cc_t c_cc[NCCS];		/* control characters */
	speed_t c_ispeed;		/* input speed */
	speed_t c_ospeed;		/* output speed */
};

struct ktermios {
	tcflag_t c_iflag;		/* input mode flags */
	tcflag_t c_oflag;		/* output mode flags */
	tcflag_t c_cflag;		/* control mode flags */
	tcflag_t c_lflag;		/* local mode flags */
	cc_t c_line;			/* line discipline */
	cc_t c_cc[NCCS];		/* control characters */
	speed_t c_ispeed;		/* input speed */
	speed_t c_ospeed;		/* output speed */
};

/* c_cc characters */
#define VINTR		 0		/* Interrupt character [ISIG] */
#define VQUIT		 1		/* Quit character [ISIG] */
#define VERASE		 2		/* Erase character [ICANON] */
#define VKILL		 3		/* Kill-line character [ICANON] */
#define VMIN		 4		/* Minimum number of bytes read at once [!ICANON] */
#define VTIME		 5		/* Time-out value (tenths of a second) [!ICANON] */
#define VEOL2		 6		/* Second EOL character [ICANON] */
#define VSWTC		 7		/* ??? */
#define VSWTCH		VSWTC
#define VSTART		 8		/* Start (X-ON) character [IXON, IXOFF] */
#define VSTOP		 9		/* Stop (X-OFF) character [IXON, IXOFF] */
#define VSUSP		10		/* Suspend character [ISIG] */
#if 0
/*
 * VDSUSP is not supported
 */
#define VDSUSP		11		/* Delayed suspend character [ISIG] */
#endif
#define VREPRINT	12		/* Reprint-line character [ICANON] */
#define VDISCARD	13		/* Discard character [IEXTEN] */
#define VWERASE		14		/* Word-erase character [ICANON] */
#define VLNEXT		15		/* Literal-next character [IEXTEN] */
#define VEOF		16		/* End-of-file character [ICANON] */
#define VEOL		17		/* End-of-line character [ICANON] */

/* c_iflag bits */
#define IUCLC	0x0200U		/* Map upper case to lower case on input */
#define IXON	0x0400U		/* Enable start/stop output control */
#define IXOFF	0x1000U		/* Enable start/stop input control */
#define IMAXBEL	0x2000U		/* Ring bell when input queue is full */
#define IUTF8	0x4000U		/* Input is UTF-8 */

/* c_oflag bits */
#define OLCUC	0x00002U	/* Map lower case to upper case on output */
#define ONLCR	0x00004U	/* Map NL to CR-NL on output */
#define NLDLY	0x00100U
#define   NL0	0x00000U
#define   NL1	0x00100U
#define CRDLY	0x00600U
#define   CR0	0x00000U
#define   CR1	0x00200U
#define   CR2	0x00400U
#define   CR3	0x00600U
#define TABDLY	0x01800U
#define   TAB0	0x00000U
#define   TAB1	0x00800U
#define   TAB2	0x01000U
#define   TAB3	0x01800U
#define   XTABS	0x01800U
#define BSDLY	0x02000U
#define   BS0	0x00000U
#define   BS1	0x02000U
#define VTDLY	0x04000U
#define   VT0	0x00000U
#define   VT1	0x04000U
#define FFDLY	0x08000U
#define   FF0	0x00000U
#define   FF1	0x08000U
/*
#define PAGEOUT ???
#define WRAP	???
 */

/* c_cflag bit meaning */
#define CBAUD		0x0000100fU
#define CSIZE		0x00000030U	/* Number of bits per byte (mask) */
#define   CS5		0x00000000U	/* 5 bits per byte */
#define   CS6		0x00000010U	/* 6 bits per byte */
#define   CS7		0x00000020U	/* 7 bits per byte */
#define   CS8		0x00000030U	/* 8 bits per byte */
#define CSTOPB		0x00000040U	/* Two stop bits instead of one */
#define CREAD		0x00000080U	/* Enable receiver */
#define PARENB		0x00000100U	/* Parity enable */
#define PARODD		0x00000200U	/* Odd parity instead of even */
#define HUPCL		0x00000400U	/* Hang up on last close */
#define CLOCAL		0x00000800U	/* Ignore modem status lines */
#define CBAUDEX		0x00001000U
#define BOTHER		0x00001000U
#define     B57600	0x00001001U
#define    B115200	0x00001002U
#define    B230400	0x00001003U
#define    B460800	0x00001004U
#define    B500000	0x00001005U
#define    B576000	0x00001006U
#define    B921600	0x00001007U
#define   B1000000	0x00001008U
#define   B1152000	0x00001009U
#define   B1500000	0x0000100aU
#define   B2000000	0x0000100bU
#define   B2500000	0x0000100cU
#define   B3000000	0x0000100dU
#define   B3500000	0x0000100eU
#define   B4000000	0x0000100fU
#define CIBAUD		0x100f0000U	/* input baud rate */

/* c_lflag bits */
#define ISIG	0x00001U	/* Enable signals */
#define ICANON	0x00002U	/* Do erase and kill processing */
#define XCASE	0x00004U
#define ECHO	0x00008U	/* Enable echo */
#define ECHOE	0x00010U	/* Visual erase for ERASE */
#define ECHOK	0x00020U	/* Echo NL after KILL */
#define ECHONL	0x00040U	/* Echo NL even if ECHO is off */
#define NOFLSH	0x00080U	/* Disable flush after interrupt */
#define IEXTEN	0x00100U	/* Enable DISCARD and LNEXT */
#define ECHOCTL	0x00200U	/* Echo control characters as ^X */
#define ECHOPRT	0x00400U	/* Hardcopy visual erase */
#define ECHOKE	0x00800U	/* Visual erase for KILL */
#define FLUSHO	0x02000U
#define PENDIN	0x04000U	/* Retype pending input (state) */
#define TOSTOP	0x08000U	/* Send SIGTTOU for background output */
#define ITOSTOP	TOSTOP
#define EXTPROC	0x10000U	/* External processing on pty */

/* ioctl (fd, TIOCSERGETLSR, &result) where result may be as below */
#define TIOCSER_TEMT	0x01U	/* Transmitter physically empty */

/* tcsetattr uses these */
#define TCSANOW		TCSETS	/* Change immediately */
#define TCSADRAIN	TCSETSW /* Change when pending output is written */
#define TCSAFLUSH	TCSETSF /* Flush pending input before changing */

#endif /* _ASM_TERMBITS_H */
