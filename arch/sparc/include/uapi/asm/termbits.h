/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_SPARC_TERMBITS_H
#define _UAPI_SPARC_TERMBITS_H

#include <asm-generic/termbits-common.h>

#if defined(__sparc__) && defined(__arch64__)
typedef unsigned int	tcflag_t;
#else
typedef unsigned long	tcflag_t;
#endif

#define NCCS 17
struct termios {
	tcflag_t c_iflag;		/* input mode flags */
	tcflag_t c_oflag;		/* output mode flags */
	tcflag_t c_cflag;		/* control mode flags */
	tcflag_t c_lflag;		/* local mode flags */
	cc_t c_line;			/* line discipline */
#ifndef __KERNEL__
	cc_t c_cc[NCCS];		/* control characters */
#else
	cc_t c_cc[NCCS+2];	/* kernel needs 2 more to hold vmin/vtime */
#define SIZEOF_USER_TERMIOS sizeof (struct termios) - (2*sizeof (cc_t))
#endif
};

struct termios2 {
	tcflag_t c_iflag;		/* input mode flags */
	tcflag_t c_oflag;		/* output mode flags */
	tcflag_t c_cflag;		/* control mode flags */
	tcflag_t c_lflag;		/* local mode flags */
	cc_t c_line;			/* line discipline */
	cc_t c_cc[NCCS+2];		/* control characters */
	speed_t c_ispeed;		/* input speed */
	speed_t c_ospeed;		/* output speed */
};

struct ktermios {
	tcflag_t c_iflag;		/* input mode flags */
	tcflag_t c_oflag;		/* output mode flags */
	tcflag_t c_cflag;		/* control mode flags */
	tcflag_t c_lflag;		/* local mode flags */
	cc_t c_line;			/* line discipline */
	cc_t c_cc[NCCS+2];		/* control characters */
	speed_t c_ispeed;		/* input speed */
	speed_t c_ospeed;		/* output speed */
};

/* c_cc characters */
#define VINTR     0
#define VQUIT     1
#define VERASE    2
#define VKILL     3
#define VEOF      4
#define VEOL      5
#define VEOL2     6
#define VSWTC     7
#define VSTART    8
#define VSTOP     9

#define VSUSP    10
#define VDSUSP   11		/* SunOS POSIX nicety I do believe... */
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15

/* Kernel keeps vmin/vtime separated, user apps assume vmin/vtime is
 * shared with eof/eol
 */
#ifndef __KERNEL__
#define VMIN     VEOF
#define VTIME    VEOL
#endif

/* c_iflag bits */
#define IUCLC	0x0200U
#define IXON	0x0400U
#define IXOFF	0x1000U
#define IMAXBEL	0x2000U
#define IUTF8   0x4000U

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
#define PAGEOUT 0x10000U		/* SUNOS specific */
#define WRAP    0x20000U		/* SUNOS specific */

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
/* We'll never see these speeds with the Zilogs, but for completeness... */
#define BOTHER		0x00001000U
#define     B57600	0x00001001U
#define    B115200	0x00001002U
#define    B230400	0x00001003U
#define    B460800	0x00001004U
/* This is what we can do with the Zilogs. */
#define     B76800	0x00001005U
/* This is what we can do with the SAB82532. */
#define    B153600	0x00001006U
#define    B307200	0x00001007U
#define    B614400	0x00001008U
#define    B921600	0x00001009U
/* And these are the rest... */
#define    B500000	0x0000100aU
#define    B576000	0x0000100bU
#define   B1000000	0x0000100cU
#define   B1152000	0x0000100dU
#define   B1500000	0x0000100eU
#define   B2000000	0x0000100fU
/* These have totally bogus values and nobody uses them
   so far. Later on we'd have to use say 0x10000x and
   adjust CBAUD constant and drivers accordingly.
#define   B2500000	0x00001010U
#define   B3000000	0x00001011U
#define   B3500000	0x00001012U
#define   B4000000	0x00001013U */
#define CIBAUD		0x100f0000U	/* input baud rate (not used) */

/* c_lflag bits */
#define ISIG	0x00000001U
#define ICANON	0x00000002U
#define XCASE	0x00000004U
#define ECHO	0x00000008U
#define ECHOE	0x00000010U
#define ECHOK	0x00000020U
#define ECHONL	0x00000040U
#define NOFLSH	0x00000080U
#define TOSTOP	0x00000100U
#define ECHOCTL	0x00000200U
#define ECHOPRT	0x00000400U
#define ECHOKE	0x00000800U
#define DEFECHO 0x00001000U		/* SUNOS thing, what is it? */
#define FLUSHO	0x00002000U
#define PENDIN	0x00004000U
#define IEXTEN	0x00008000U
#define EXTPROC	0x00010000U

/* modem lines */
#define TIOCM_LE	0x001U
#define TIOCM_DTR	0x002U
#define TIOCM_RTS	0x004U
#define TIOCM_ST	0x008U
#define TIOCM_SR	0x010U
#define TIOCM_CTS	0x020U
#define TIOCM_CAR	0x040U
#define TIOCM_RNG	0x080U
#define TIOCM_DSR	0x100U
#define TIOCM_CD	TIOCM_CAR
#define TIOCM_RI	TIOCM_RNG
#define TIOCM_OUT1	0x2000U
#define TIOCM_OUT2	0x4000U
#define TIOCM_LOOP	0x8000U

/* ioctl (fd, TIOCSERGETLSR, &result) where result may be as below */
#define TIOCSER_TEMT    0x01U	/* Transmitter physically empty */

/* tcsetattr uses these */
#define TCSANOW		0
#define TCSADRAIN	1
#define TCSAFLUSH	2

#endif /* _UAPI_SPARC_TERMBITS_H */
