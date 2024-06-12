/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef __ARCH_PARISC_TERMBITS_H__
#define __ARCH_PARISC_TERMBITS_H__

#include <asm-generic/termbits-common.h>

typedef unsigned int	tcflag_t;

#define NCCS 19
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
#define VINTR		 0
#define VQUIT		 1
#define VERASE		 2
#define VKILL		 3
#define VEOF		 4
#define VTIME		 5
#define VMIN		 6
#define VSWTC		 7
#define VSTART		 8
#define VSTOP		 9
#define VSUSP		10
#define VEOL		11
#define VREPRINT	12
#define VDISCARD	13
#define VWERASE		14
#define VLNEXT		15
#define VEOL2		16

/* c_iflag bits */
#define IUCLC	0x0200U
#define IXON	0x0400U
#define IXOFF	0x1000U
#define IMAXBEL	0x4000U
#define IUTF8	0x8000U

/* c_oflag bits */
#define OLCUC	0x00002U
#define ONLCR	0x00004U
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

/* c_cflag bit meaning */
#define CBAUD		0x0000100fU
#define CSIZE		0x00000030U
#define   CS5		0x00000000U
#define   CS6		0x00000010U
#define   CS7		0x00000020U
#define   CS8		0x00000030U
#define CSTOPB		0x00000040U
#define CREAD		0x00000080U
#define PARENB		0x00000100U
#define PARODD		0x00000200U
#define HUPCL		0x00000400U
#define CLOCAL		0x00000800U
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
#define CIBAUD		0x100f0000U		/* input baud rate */

/* c_lflag bits */
#define ISIG	0x00001U
#define ICANON	0x00002U
#define XCASE	0x00004U
#define ECHO	0x00008U
#define ECHOE	0x00010U
#define ECHOK	0x00020U
#define ECHONL	0x00040U
#define NOFLSH	0x00080U
#define TOSTOP	0x00100U
#define ECHOCTL	0x00200U
#define ECHOPRT	0x00400U
#define ECHOKE	0x00800U
#define FLUSHO	0x01000U
#define PENDIN	0x04000U
#define IEXTEN	0x08000U
#define EXTPROC	0x10000U

/* tcsetattr uses these */
#define	TCSANOW		0
#define	TCSADRAIN	1
#define	TCSAFLUSH	2

#endif
