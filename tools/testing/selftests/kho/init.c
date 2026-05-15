// SPDX-License-Identifier: GPL-2.0

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <linux/kexec.h>

/* from arch/x86/include/asm/setup.h */
#define COMMAND_LINE_SIZE	2048

#define KERNEL_IMAGE "/kernel"
#define INITRD_IMAGE "/initrd"

void console_log(char *log)
{
	static int fd;

	if (!fd)
		fd = open("/dev/console", O_WRONLY);

	write(fd, log, strlen(log) + 1);
}

static int mount_filesystems(void)
{
	if (mount("debugfs", "/debugfs", "debugfs", 0, NULL) < 0)
		return -1;

	return mount("proc", "/proc", "proc", 0, NULL);
}

static long kexec_file_load(int kernel_fd, int initrd_fd,
			    unsigned long cmdline_len, const char *cmdline,
			    unsigned long flags)
{
	return syscall(__NR_kexec_file_load, kernel_fd, initrd_fd, cmdline_len,
		       cmdline, flags);
}

#define CMDLINE_SECOND " test_kho.second_boot=1"
#define CMDLINE_THIRD " test_kho.third_boot=1"

static int kexec_load(void)
{
	char cmdline[COMMAND_LINE_SIZE];
	ssize_t len;
	int fd, err, initfd;

	fd = open("/proc/cmdline", O_RDONLY);
	if (fd < 0)
		return -1;

	len = read(fd, cmdline, sizeof(cmdline));
	close(fd);
	if (len < 0)
		return -1;

	/* replace \n with \0 */
	cmdline[len - 1] = 0;
	fd = open(KERNEL_IMAGE, O_RDONLY);
	if (fd < 0)
		return -1;

	if (!strstr(cmdline, CMDLINE_SECOND)) {
		console_log("init: first boot detected\n");

		strcat(cmdline, CMDLINE_SECOND);
		len = strlen(cmdline) + 1;
		initfd = open(INITRD_IMAGE, O_RDONLY);
		if (initfd < 0)
			return -1;

		err = kexec_file_load(fd, initfd, len, cmdline, 0);
		close(fd);
		close(initfd);
		return err ? : 0;
	}

	console_log("init: second boot detected\n");
	strcat(cmdline, CMDLINE_THIRD);
	len = strlen(cmdline) + 1;

	err = kexec_file_load(fd, -1, len, cmdline, KEXEC_FILE_NO_INITRAMFS);
	close(fd);

	return err ? : 0;
}

int main(int argc, char *argv[])
{
	if (mount_filesystems())
		goto err_reboot;

	if (kexec_load())
		goto err_reboot;

	if (reboot(RB_KEXEC))
		goto err_reboot;

	return 0;

err_reboot:
	reboot(RB_AUTOBOOT);
	return -1;
}
