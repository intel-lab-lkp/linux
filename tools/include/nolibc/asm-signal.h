/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * ASM signal definitions for NOLIBC
 */

#ifndef _NOLIBC_ASM_SIGNAL_H
#define _NOLIBC_ASM_SIGNAL_H

/*
 * This reproduces the kernel headers for the different architectures.
 */

#include <linux/types.h>

#if defined(__mips__)
#define _NSIG		128
#else
#define _NSIG		64
#endif
#define _NSIG_BPW	__BITS_PER_LONG
#define _NSIG_WORDS	(_NSIG / _NSIG_BPW)

typedef struct {
	unsigned long sig[_NSIG_WORDS];
} sigset_t;

#if defined(__mips__)
#define SIGHUP		 1	/* Hangup (POSIX).  */
#define SIGINT		 2	/* Interrupt (ANSI).  */
#define SIGQUIT		 3	/* Quit (POSIX).  */
#define SIGILL		 4	/* Illegal instruction (ANSI).	*/
#define SIGTRAP		 5	/* Trace trap (POSIX).	*/
#define SIGIOT		 6	/* IOT trap (4.2 BSD).	*/
#define SIGABRT		 SIGIOT /* Abort (ANSI).  */
#define SIGEMT		 7
#define SIGFPE		 8	/* Floating-point exception (ANSI).  */
#define SIGKILL		 9	/* Kill, unblockable (POSIX).  */
#define SIGBUS		10	/* BUS error (4.2 BSD).	 */
#define SIGSEGV		11	/* Segmentation violation (ANSI).  */
#define SIGSYS		12
#define SIGPIPE		13	/* Broken pipe (POSIX).	 */
#define SIGALRM		14	/* Alarm clock (POSIX).	 */
#define SIGTERM		15	/* Termination (ANSI).	*/
#define SIGUSR1		16	/* User-defined signal 1 (POSIX).  */
#define SIGUSR2		17	/* User-defined signal 2 (POSIX).  */
#define SIGCHLD		18	/* Child status has changed (POSIX).  */
#define SIGCLD		SIGCHLD /* Same as SIGCHLD (System V).	*/
#define SIGPWR		19	/* Power failure restart (System V).  */
#define SIGWINCH	20	/* Window size change (4.3 BSD, Sun).  */
#define SIGURG		21	/* Urgent condition on socket (4.2 BSD).  */
#define SIGIO		22	/* I/O now possible (4.2 BSD).	*/
#define SIGPOLL		SIGIO	/* Pollable event occurred (System V).	*/
#define SIGSTOP		23	/* Stop, unblockable (POSIX).  */
#define SIGTSTP		24	/* Keyboard stop (POSIX).  */
#define SIGCONT		25	/* Continue (POSIX).  */
#define SIGTTIN		26	/* Background read from tty (POSIX).  */
#define SIGTTOU		27	/* Background write to tty (POSIX).  */
#define SIGVTALRM	28	/* Virtual alarm clock (4.2 BSD).  */
#define SIGPROF		29	/* Profiling alarm clock (4.2 BSD).  */
#define SIGXCPU		30	/* CPU limit exceeded (4.2 BSD).  */
#define SIGXFSZ		31	/* File size limit exceeded (4.2 BSD).	*/

#elif defined(__sparc__)
/* On the Sparc the signal handlers get passed a 'sub-signal' code
 * for certain signal types, which we document here.
 */
#define SIGHUP		 1
#define SIGINT		 2
#define SIGQUIT		 3
#define SIGILL		 4
#define    SUBSIG_STACK       0
#define    SUBSIG_ILLINST     2
#define    SUBSIG_PRIVINST    3
#define    SUBSIG_BADTRAP(t)  (0x80 + (t))

#define SIGTRAP		 5
#define SIGABRT		 6
#define SIGIOT		 6

#define SIGEMT           7
#define    SUBSIG_TAG    10

#define SIGFPE		 8
#define    SUBSIG_FPDISABLED     0x400
#define    SUBSIG_FPERROR        0x404
#define    SUBSIG_FPINTOVFL      0x001
#define    SUBSIG_FPSTSIG        0x002
#define    SUBSIG_IDIVZERO       0x014
#define    SUBSIG_FPINEXACT      0x0c4
#define    SUBSIG_FPDIVZERO      0x0c8
#define    SUBSIG_FPUNFLOW       0x0cc
#define    SUBSIG_FPOPERROR      0x0d0
#define    SUBSIG_FPOVFLOW       0x0d4

#define SIGKILL		 9
#define SIGBUS          10
#define    SUBSIG_BUSTIMEOUT    1
#define    SUBSIG_ALIGNMENT     2
#define    SUBSIG_MISCERROR     5

#define SIGSEGV		11
#define    SUBSIG_NOMAPPING     3
#define    SUBSIG_PROTECTION    4
#define    SUBSIG_SEGERROR      5

#define SIGSYS		12

#define SIGPIPE		13
#define SIGALRM		14
#define SIGTERM		15
#define SIGURG          16

/* SunOS values which deviate from the Linux/i386 ones */
#define SIGSTOP		17
#define SIGTSTP		18
#define SIGCONT		19
#define SIGCHLD		20
#define SIGTTIN		21
#define SIGTTOU		22
#define SIGIO		23
#define SIGPOLL		SIGIO   /* SysV name for SIGIO */
#define SIGXCPU		24
#define SIGXFSZ		25
#define SIGVTALRM	26
#define SIGPROF		27
#define SIGWINCH	28
#define SIGLOST		29
#define SIGPWR		SIGLOST
#define SIGUSR1		30
#define SIGUSR2		31

#else /* asm-generic signal definitions */
#define SIGHUP		 1
#define SIGINT		 2
#define SIGQUIT		 3
#define SIGILL		 4
#define SIGTRAP		 5
#define SIGABRT		 6
#define SIGIOT		 6
#define SIGBUS		 7
#define SIGFPE		 8
#define SIGKILL		 9
#define SIGUSR1		10
#define SIGSEGV		11
#define SIGUSR2		12
#define SIGPIPE		13
#define SIGALRM		14
#define SIGTERM		15
#define SIGSTKFLT	16
#define SIGCHLD		17
#define SIGCONT		18
#define SIGSTOP		19
#define SIGTSTP		20
#define SIGTTIN		21
#define SIGTTOU		22
#define SIGURG		23
#define SIGXCPU		24
#define SIGXFSZ		25
#define SIGVTALRM	26
#define SIGPROF		27
#define SIGWINCH	28
#define SIGIO		29
#define SIGPOLL		SIGIO
/*
#define SIGLOST		29
*/
#define SIGPWR		30
#define SIGSYS		31
#define	SIGUNUSED	31
#endif

/* These should not be considered constants from userland.  */
#define SIGRTMIN	32
#ifndef SIGRTMAX
#define SIGRTMAX	_NSIG
#endif

#if !defined MINSIGSTKSZ || !defined SIGSTKSZ
#if defined(__aarch64__)
#define MINSIGSTKSZ	 5120
#define SIGSTKSZ	16384
#elif defined(__loongarch__) || defined(__sparc__)
#define MINSIGSTKSZ	 4096
#define SIGSTKSZ	16384
#elif defined(__powerpc64__)
#define MINSIGSTKSZ	 8192
#define SIGSTKSZ	32768
#else
#define MINSIGSTKSZ	 2048
#define SIGSTKSZ	 8192
#endif
#endif

#if defined(__mips__)
#define SA_ONSTACK      0x08000000
#define SA_RESETHAND    0x80000000
#define SA_RESTART      0x10000000
#define SA_SIGINFO      0x00000008
#define SA_NODEFER      0x40000000
#define SA_NOCLDWAIT    0x00010000
#define SA_NOCLDSTOP    0x00000001

#elif defined(__sparc__)
#define _SV_SSTACK    1u
#define _SV_INTR      2u
#define _SV_RESET     4u
#define _SV_IGNCHILD  8u

#define SA_NOCLDSTOP    _SV_IGNCHILD
#define SA_STACK        _SV_SSTACK
#define SA_ONSTACK      _SV_SSTACK
#define SA_RESTART      _SV_INTR
#define SA_RESETHAND    _SV_RESET
#define SA_NODEFER      0x20u
#define SA_NOCLDWAIT    0x100u
#define SA_SIGINFO      0x200u

#else
#define SA_NOCLDSTOP    0x00000001
#define SA_NOCLDWAIT    0x00000002
#define SA_SIGINFO      0x00000004
#define SA_UNSUPPORTED  0x00000400
#define SA_EXPOSE_TAGBITS       0x00000800
#define SA_ONSTACK      0x08000000
#define SA_RESTART      0x10000000
#define SA_NODEFER      0x40000000
#define SA_RESETHAND    0x80000000
#endif

#define SA_NOMASK       SA_NODEFER
#define SA_ONESHOT      SA_RESETHAND

#if defined(__ARM_EABI__) || defined(__aarch64__) || defined(__m68k__) || defined(__powerpc__) || defined(__s390x__) || defined(__s390__) || defined(__sh__) || defined(__i386__) || defined(__x86_64__)
#define SA_RESTORER	0x04000000
#endif

#endif /* _NOLIBC_ASM_SIGNAL_H */