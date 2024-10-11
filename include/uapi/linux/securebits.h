/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_SECUREBITS_H
#define _UAPI_LINUX_SECUREBITS_H

/* Each securesetting is implemented using two bits. One bit specifies
   whether the setting is on or off. The other bit specify whether the
   setting is locked or not. A setting which is locked cannot be
   changed from user-level. */
#define issecure_mask(X)	(1 << (X))

#define SECUREBITS_DEFAULT 0x00000000

/* When set UID 0 has no special privileges. When unset, we support
   inheritance of root-permissions and suid-root executable under
   compatibility mode. We raise the effective and inheritable bitmasks
   *of the executable file* if the effective uid of the new process is
   0. If the real uid is 0, we raise the effective (legacy) bit of the
   executable file. */
#define SECURE_NOROOT			0
#define SECURE_NOROOT_LOCKED		1  /* make bit-0 immutable */

#define SECBIT_NOROOT		(issecure_mask(SECURE_NOROOT))
#define SECBIT_NOROOT_LOCKED	(issecure_mask(SECURE_NOROOT_LOCKED))

/* When set, setuid to/from uid 0 does not trigger capability-"fixup".
   When unset, to provide compatiblility with old programs relying on
   set*uid to gain/lose privilege, transitions to/from uid 0 cause
   capabilities to be gained/lost. */
#define SECURE_NO_SETUID_FIXUP		2
#define SECURE_NO_SETUID_FIXUP_LOCKED	3  /* make bit-2 immutable */

#define SECBIT_NO_SETUID_FIXUP	(issecure_mask(SECURE_NO_SETUID_FIXUP))
#define SECBIT_NO_SETUID_FIXUP_LOCKED \
			(issecure_mask(SECURE_NO_SETUID_FIXUP_LOCKED))

/* When set, a process can retain its capabilities even after
   transitioning to a non-root user (the set-uid fixup suppressed by
   bit 2). Bit-4 is cleared when a process calls exec(); setting both
   bit 4 and 5 will create a barrier through exec that no exec()'d
   child can use this feature again. */
#define SECURE_KEEP_CAPS		4
#define SECURE_KEEP_CAPS_LOCKED		5  /* make bit-4 immutable */

#define SECBIT_KEEP_CAPS	(issecure_mask(SECURE_KEEP_CAPS))
#define SECBIT_KEEP_CAPS_LOCKED (issecure_mask(SECURE_KEEP_CAPS_LOCKED))

/* When set, a process cannot add new capabilities to its ambient set. */
#define SECURE_NO_CAP_AMBIENT_RAISE		6
#define SECURE_NO_CAP_AMBIENT_RAISE_LOCKED	7  /* make bit-6 immutable */

#define SECBIT_NO_CAP_AMBIENT_RAISE (issecure_mask(SECURE_NO_CAP_AMBIENT_RAISE))
#define SECBIT_NO_CAP_AMBIENT_RAISE_LOCKED \
			(issecure_mask(SECURE_NO_CAP_AMBIENT_RAISE_LOCKED))

/*
 * The SECBIT_EXEC_RESTRICT_FILE and SECBIT_EXEC_DENY_INTERACTIVE securebits
 * are intended for script interpreters and dynamic linkers to enforce a
 * consistent execution security policy handled by the kernel.
 *
 * Whether an interpreter should check these securebits or not depends on the
 * security risk of running malicious scripts with respect to the execution
 * environment, and whether the kernel can check if a script is trustworthy or
 * not.  For instance, Python scripts running on a server can use arbitrary
 * syscalls and access arbitrary files.  Such interpreters should then be
 * enlighten to use these securebits and let users define their security
 * policy.  However, a JavaScript engine running in a web browser should
 * already be sandboxed and then should not be able to harm the user's
 * environment.
 *
 * When SECBIT_EXEC_RESTRICT_FILE is set, a process should only interpret or
 * execute a file if a call to execveat(2) with the related file descriptor and
 * the AT_CHECK flag succeed.
 *
 * This secure bit may be set by user session managers, service managers,
 * container runtimes, sandboxer tools...  Except for test environments, the
 * related SECBIT_EXEC_RESTRICT_FILE_LOCKED bit should also be set.
 *
 * Programs should only enforce consistent restrictions according to the
 * securebits but without relying on any other user-controlled configuration.
 * Indeed, the use case for these securebits is to only trust executable code
 * vetted by the system configuration (through the kernel), so we should be
 * careful to not let untrusted users control this configuration.
 *
 * However, script interpreters may still use user configuration such as
 * environment variables as long as it is not a way to disable the securebits
 * checks.  For instance, the PATH and LD_PRELOAD variables can be set by a
 * script's caller.  Changing these variables may lead to unintended code
 * executions, but only from vetted executable programs, which is OK.  For this
 * to make sense, the system should provide a consistent security policy to
 * avoid arbitrary code execution e.g., by enforcing a write xor execute
 * policy.
 *
 * SECBIT_EXEC_RESTRICT_FILE is complementary and should also be checked.
 */
#define SECURE_EXEC_RESTRICT_FILE		8
#define SECURE_EXEC_RESTRICT_FILE_LOCKED	9  /* make bit-8 immutable */

#define SECBIT_EXEC_RESTRICT_FILE (issecure_mask(SECURE_EXEC_RESTRICT_FILE))
#define SECBIT_EXEC_RESTRICT_FILE_LOCKED \
			(issecure_mask(SECURE_EXEC_RESTRICT_FILE_LOCKED))

/*
 * When SECBIT_EXEC_DENY_INTERACTIVE is set, a process should never interpret
 * interactive user commands (e.g. scripts).  However, if such commands are
 * passed through a file descriptor (e.g. stdin), its content should be
 * interpreted if a call to execveat(2) with the related file descriptor and
 * the AT_CHECK flag succeed.
 *
 * For instance, script interpreters called with a script snippet as argument
 * should always deny such execution if SECBIT_EXEC_DENY_INTERACTIVE is set.
 *
 * This secure bit may be set by user session managers, service managers,
 * container runtimes, sandboxer tools...  Except for test environments, the
 * related SECBIT_EXEC_DENY_INTERACTIVE_LOCKED bit should also be set.
 *
 * See the SECBIT_EXEC_RESTRICT_FILE documentation.
 *
 * Here is the expected behavior for a script interpreter according to
 * combination of any exec securebits:
 *
 * 1. SECURE_EXEC_RESTRICT_FILE=0 SECURE_EXEC_DENY_INTERACTIVE=0 (default)
 *    Always interpret scripts, and allow arbitrary user commands.
 *    => No threat, everyone and everything is trusted, but we can get ahead of
 *       potential issues thanks to the call to execveat with AT_CHECK which
 *       should always be performed but ignored by the script interpreter.
 *       Indeed, this check is still important to enable systems administrators
 *       to verify requests (e.g. with audit) and prepare for migration to a
 *       secure mode.
 *
 * 2. SECURE_EXEC_RESTRICT_FILE=1 SECURE_EXEC_DENY_INTERACTIVE=0
 *    Deny script interpretation if they are not executable, but allow
 *    arbitrary user commands.
 *    => The threat is (potential) malicious scripts run by trusted (and not
 *       fooled) users.  That can protect against unintended script executions
 *       (e.g. sh /tmp/*.sh).  This makes sense for (semi-restricted) user
 *       sessions.
 *
 * 3. SECURE_EXEC_RESTRICT_FILE=0 SECURE_EXEC_DENY_INTERACTIVE=1
 *    Always interpret scripts, but deny arbitrary user commands.
 *    => This use case may be useful for secure services (i.e. without
 *       interactive user session) where scripts' integrity is verified (e.g.
 *       with IMA/EVM or dm-verity/IPE) but where access rights might not be
 *       ready yet.  Indeed, arbitrary interactive commands would be much more
 *       difficult to check.
 *
 * 4. SECURE_EXEC_RESTRICT_FILE=1 SECURE_EXEC_DENY_INTERACTIVE=1
 *    Deny script interpretation if they are not executable, and also deny
 *    any arbitrary user commands.
 *    => The threat is malicious scripts run by untrusted users (but trusted
 *       code).  This makes sense for system services that may only execute
 *       trusted scripts.
 */
#define SECURE_EXEC_DENY_INTERACTIVE		10
#define SECURE_EXEC_DENY_INTERACTIVE_LOCKED	11  /* make bit-10 immutable */

#define SECBIT_EXEC_DENY_INTERACTIVE \
			(issecure_mask(SECURE_EXEC_DENY_INTERACTIVE))
#define SECBIT_EXEC_DENY_INTERACTIVE_LOCKED \
			(issecure_mask(SECURE_EXEC_DENY_INTERACTIVE_LOCKED))

#define SECURE_ALL_BITS		(issecure_mask(SECURE_NOROOT) | \
				 issecure_mask(SECURE_NO_SETUID_FIXUP) | \
				 issecure_mask(SECURE_KEEP_CAPS) | \
				 issecure_mask(SECURE_NO_CAP_AMBIENT_RAISE) | \
				 issecure_mask(SECURE_EXEC_RESTRICT_FILE) | \
				 issecure_mask(SECURE_EXEC_DENY_INTERACTIVE))
#define SECURE_ALL_LOCKS	(SECURE_ALL_BITS << 1)

#define SECURE_ALL_UNPRIVILEGED (issecure_mask(SECURE_EXEC_RESTRICT_FILE) | \
				 issecure_mask(SECURE_EXEC_DENY_INTERACTIVE))

#endif /* _UAPI_LINUX_SECUREBITS_H */
