// SPDX-License-Identifier: GPL-2.0-only
/*
 * POEG - example userspace application
 * Copyright (C) 2023 Biju Das
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <poll.h>

#include <linux/poeg.h>

int main(int argc, char *argv[])
{
	struct poeg_event event_data;
	struct poeg_cmd cmd;
	unsigned int val;
	long cmd_val;
	int ret, fd;
	char *p;
	int i;

	cmd_val = strtol(argv[1], &p, 10);
	if (*p != '\0' || errno != 0)
		return 1; // In main(), returning non-zero means failure

	fd = open("/dev/poeg3", O_RDWR);
	if (fd < 0)
		perror("open");
	else
		printf("[POEG]open\n");

	if (cmd_val == RZG2L_POEG_OUTPUT_DISABLE_USR_ENABLE_CMD ||
	    cmd_val == RZG2L_POEG_OUTPUT_DISABLE_USR_DISABLE_CMD) {
		if (cmd_val == RZG2L_POEG_OUTPUT_DISABLE_USR_ENABLE_CMD)
			printf("[POEG] user control pin output disable enabled\n");
		else
			printf("[POEG] user control pin output disable disabled\n");

		cmd.val = cmd_val;
		cmd.channel = 4;
		ret = write(fd, &cmd, sizeof(cmd));
		if (ret == -1) {
			perror("Failed to write cmd data");
			return 1;
		}
	} else {
		printf("[POEG] GPT control configure IRQ\n");
		cmd.val = RZG2L_POEG_GPT_CFG_IRQ_CMD;
		cmd.channel = 4;
		ret = write(fd, &cmd, sizeof(cmd));
		if (ret == -1) {
			perror("Failed to write cmd data");
			return 1;
		}

		for (;;) {
			ret = read(fd, &event_data, sizeof(event_data));
			if (ret == -1) {
				perror("Failed to read event data");
				return 1;
			}

			val = event_data.gpt_disable_irq_status;
			if (val) {
				/* emulate fault clearing condition by adding delay */
				sleep(2);
				for (i = 0; i < 8; i++) {
					if (val & 7) {
						printf("gpt ch:%u, irq=%x\n", i, val & 7);
						cmd.val = RZG2L_POEG_GPT_FAULT_CLR_CMD;
						cmd.channel = 4;
						ret = write(fd, &cmd, sizeof(cmd));
					}
					val >>= 3;
				}
			}
		}
	}

	if (close(fd) != 0)
		perror("close");
	else
		printf("[POEG]close\n");

	return 0;
}
