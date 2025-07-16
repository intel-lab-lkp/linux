#ifndef _ASM_GENERIC_KTERMIOS_H
#define _ASM_GENERIC_KTERMIOS_H

#ifndef KNCCS
# define KNCCS NCCS
#endif

struct ktermios {
	tcflag_t c_iflag;		/* input mode flags */
	tcflag_t c_oflag;		/* output mode flags */
	tcflag_t c_cflag;		/* control mode flags */
	tcflag_t c_lflag;		/* local mode flags */
#ifndef KTERMIOS_C_CC_BEFORE_C_LINE
	/* Most architectures */
	cc_t c_line;			/* line discipline */
	cc_t c_cc[KNCCS];		/* control characters */
#else
	/* Alpha and PowerPC */
	cc_t c_cc[KNCCS];		/* control characters */
	cc_t c_line;			/* line discipline */
#endif
	speed_t c_ispeed;		/* input speed */
	speed_t c_ospeed;		/* output speed */
};

#endif /* _ASM_GENERIC_KTERMIOS_H */
