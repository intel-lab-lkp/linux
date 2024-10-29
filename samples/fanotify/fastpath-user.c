// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/fanotify.h>
#include <unistd.h>
#include <sys/ioctl.h>

static int total_event_cnt;

static void handle_notifications(char *buffer, int len)
{
	struct fanotify_event_metadata *event =
		(struct fanotify_event_metadata *) buffer;
	struct fanotify_event_info_header *info;
	struct fanotify_event_info_fid *fid;
	struct file_handle *handle;
	char *name;
	int off;

	for (; FAN_EVENT_OK(event, len); event = FAN_EVENT_NEXT(event, len)) {
		for (off = sizeof(*event) ; off < event->event_len;
		     off += info->len) {
			info = (struct fanotify_event_info_header *)
				((char *) event + off);
			switch (info->info_type) {
			case FAN_EVENT_INFO_TYPE_DFID_NAME:
				fid = (struct fanotify_event_info_fid *) info;
				handle = (struct file_handle *)&fid->handle;
				name = (char *)handle + sizeof(*handle) + handle->handle_bytes;

				printf("Accessing file %s\n", name);
				total_event_cnt++;
				break;
			default:
				break;
			}
		}
	}
}

int main(int argc, char **argv)
{
	struct fanotify_fastpath_args args = {
		.name = "ignore-prefix",
		.version = 1,
		.flags = 0,
	};
	char buffer[BUFSIZ];
	int fd;

	if (argc < 3) {
		printf("Usage\n"
		       "\t %s <path to monitor> <prefix to ignore>\n",
			argv[0]);
		return 1;
	}

	args.init_args = (__u64)argv[2];
	args.init_args_len = strlen(argv[2]) + 1;

	fd = fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_NAME | FAN_REPORT_DIR_FID, O_RDONLY);
	if (fd < 0)
		errx(1, "fanotify_init");

	if (fanotify_mark(fd, FAN_MARK_ADD,
			  FAN_OPEN | FAN_ONDIR | FAN_EVENT_ON_CHILD,
			  AT_FDCWD, argv[1])) {
		errx(1, "fanotify_mark");
	}

	if (ioctl(fd, FAN_IOC_ADD_FP, &args))
		errx(1, "ioctl");

	while (total_event_cnt < 10) {
		int n = read(fd, buffer, BUFSIZ);

		if (n < 0)
			errx(1, "read");

		handle_notifications(buffer, n);
	}

	ioctl(fd, FAN_IOC_DEL_FP);
	close(fd);

	return 0;
}
