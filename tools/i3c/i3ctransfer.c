// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 Synopsys, Inc. and/or its affiliates.
 * Copyright (c) 2026 Google LLC
 *
 * Author: Vitor Soares <vitor.soares@synopsys.com>
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/i3c/i3cdev.h>

#define VERSION "0.2"

static const char *sopts = "d:m:c:r:w:vh";
static const struct option lopts[] = {
	{"device",		required_argument,	NULL,	'd' },
	{"mode",		required_argument,	NULL,	'm' },
	{"command",		required_argument,	NULL,	'c' },
	{"read",		required_argument,	NULL,	'r' },
	{"write",		required_argument,	NULL,	'w' },
	{"help",		no_argument,		NULL,	'h' },
	{"version",		no_argument,		NULL,	'v' },
	{0, 0, 0, 0}
};

static void print_usage(const char *name)
{
	fprintf(stderr, "usage: %s options...\n", name);
	fprintf(stderr, "  options:\n");
	fprintf(stderr, "    -d --device  <dev>    device to use.\n");
	fprintf(stderr,
		"    -m --mode    <mode>   sdr (default), hdr-ddr, hdr-tsp, hdr-tsl.\n");
	fprintf(stderr,
		"    -c --command <cmd>    HDR command byte (0x00-0x7f W, 0x80-0xff R).\n");
	fprintf(stderr, "    -r --read    <len>    read data length in bytes.\n");
	fprintf(stderr,
		"    -w --write   <block>  write comma-separated bytes (e.g. 0x01,0x02).\n");
	fprintf(stderr, "    -h --help             output usage message and exit.\n");
	fprintf(stderr, "    -v --version          output version number and exit.\n");
}

static int parse_mode(const char *arg, uint8_t *mode)
{
	char *endptr;
	long val;

	if (!strcmp(arg, "sdr")) {
		*mode = I3C_XFER_MODE_SDR;
		return 0;
	}
	if (!strcmp(arg, "hdr-ddr") || !strcmp(arg, "ddr")) {
		*mode = I3C_XFER_MODE_HDR_DDR;
		return 0;
	}
	if (!strcmp(arg, "hdr-tsp") || !strcmp(arg, "tsp")) {
		*mode = I3C_XFER_MODE_HDR_TSP;
		return 0;
	}
	if (!strcmp(arg, "hdr-tsl") || !strcmp(arg, "tsl")) {
		*mode = I3C_XFER_MODE_HDR_TSL;
		return 0;
	}

	val = strtol(arg, &endptr, 0);
	if (*endptr == '\0' &&
	    (val == I3C_XFER_MODE_HDR_DDR ||
	     val == I3C_XFER_MODE_HDR_TSP ||
	     val == I3C_XFER_MODE_HDR_TSL ||
	     val == I3C_XFER_MODE_SDR)) {
		*mode = (uint8_t)val;
		return 0;
	}

	fprintf(stderr, "Error: invalid transfer mode '%s'\n", arg);
	return -1;
}

static int rx_args_to_xfer(struct i3c_ioc_xfer *xfer, char *arg,
			   uint8_t mode, int cmd)
{
	char *endptr;
	uint8_t *tmp;
	long len;

	len = strtol(arg, &endptr, 0);
	if (*endptr != '\0' || len <= 0 || len > UINT16_MAX) {
		fprintf(stderr, "Error: invalid read length '%s'\n", arg);
		return -1;
	}

	if (mode == I3C_XFER_MODE_SDR) {
		xfer->rnw = I3C_DEV_DIR_READ;
	} else {
		if (cmd < 0 || !(cmd & 0x80)) {
			fprintf(stderr,
				"Error: HDR read requires command (-c) in range 0x80..0xff\n");
			return -1;
		}
		xfer->cmd = (uint8_t)cmd;
	}

	tmp = calloc(len, sizeof(uint8_t));
	if (!tmp)
		return -1;

	xfer->mode = mode;
	xfer->len = (uint16_t)len;
	xfer->data = (uintptr_t)tmp;

	return 0;
}

static int w_args_to_xfer(struct i3c_ioc_xfer *xfer, char *arg,
			  uint8_t mode, int cmd)
{
	char *data_ptrs[256];
	int len, i = 0;
	uint8_t *tmp;

	if (mode == I3C_XFER_MODE_SDR) {
		xfer->rnw = I3C_DEV_DIR_WRITE;
	} else {
		if (cmd < 0 || (cmd & 0x80)) {
			fprintf(stderr,
				"Error: HDR write requires command (-c) in range 0x00..0x7f\n");
			return -1;
		}
		xfer->cmd = (uint8_t)cmd;
	}

	data_ptrs[i] = strtok(arg, ",");
	while (data_ptrs[i] && i < 255)
		data_ptrs[++i] = strtok(NULL, ",");

	if (i == 0) {
		fprintf(stderr, "Error: empty write data block\n");
		return -1;
	}

	tmp = calloc(i, sizeof(uint8_t));
	if (!tmp)
		return -1;

	for (len = 0; len < i; len++) {
		char *endptr;
		long val = strtol(data_ptrs[len], &endptr, 0);

		if (*endptr != '\0' || val < 0 || val > UINT8_MAX) {
			fprintf(stderr, "Error: invalid byte value '%s'\n",
				data_ptrs[len]);
			free(tmp);
			return -1;
		}
		tmp[len] = (uint8_t)val;
	}

	xfer->mode = mode;
	xfer->len = (uint16_t)len;
	xfer->data = (uintptr_t)tmp;

	return 0;
}

static void print_rx_data(const struct i3c_ioc_xfer *xfer)
{
	const uint8_t *tmp = (const uint8_t *)(uintptr_t)xfer->data;
	int i;

	fprintf(stdout, "  received data (%u bytes):\n", xfer->actual_len);
	for (i = 0; i < xfer->actual_len; i++)
		fprintf(stdout, "    0x%02x\n", tmp[i]);
}

int main(int argc, char *argv[])
{
	uint8_t mode = I3C_XFER_MODE_SDR;
	struct i3c_ioc_xfer *xfers;
	char *device = NULL;
	int file, ret, opt, i;
	int nxfers = 0;
	int cmd = -1;

	while ((opt = getopt_long(argc, argv, sopts, lopts, NULL)) != EOF) {
		switch (opt) {
		case 'h':
			print_usage(argv[0]);
			return EXIT_SUCCESS;
		case 'v':
			fprintf(stderr, "%s - %s\n", argv[0], VERSION);
			return EXIT_SUCCESS;
		case 'd':
			device = optarg;
			break;
		case 'm':
			if (parse_mode(optarg, &mode))
				return EXIT_FAILURE;
			break;
		case 'c': {
			char *endptr;
			long val = strtol(optarg, &endptr, 0);

			if (*endptr != '\0' || val < 0 || val > UINT8_MAX) {
				fprintf(stderr, "Error: invalid command '%s'\n",
					optarg);
				return EXIT_FAILURE;
			}
			cmd = (int)val;
			break;
		}
		case 'r':
		case 'w':
			nxfers++;
			break;
		default:
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (!device || nxfers == 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	file = open(device, O_RDWR);
	if (file < 0) {
		fprintf(stderr, "Error: failed to open %s: %s\n",
			device, strerror(errno));
		return EXIT_FAILURE;
	}

	xfers = calloc(nxfers, sizeof(*xfers));
	if (!xfers) {
		close(file);
		return EXIT_FAILURE;
	}

	optind = 1;
	nxfers = 0;
	cmd = -1;

	while ((opt = getopt_long(argc, argv, sopts, lopts, NULL)) != EOF) {
		switch (opt) {
		case 'h':
		case 'v':
		case 'd':
		case 'm':
			break;
		case 'c':
			cmd = (int)strtol(optarg, NULL, 0);
			break;
		case 'r':
			if (rx_args_to_xfer(&xfers[nxfers], optarg, mode, cmd)) {
				ret = EXIT_FAILURE;
				goto err_free;
			}
			nxfers++;
			break;
		case 'w':
			if (w_args_to_xfer(&xfers[nxfers], optarg, mode, cmd)) {
				ret = EXIT_FAILURE;
				goto err_free;
			}
			nxfers++;
			break;
		}
	}

	if (ioctl(file, I3C_IOC_XFER(nxfers), xfers) < 0) {
		fprintf(stderr, "Error: transfer failed: %s\n", strerror(errno));
		ret = EXIT_FAILURE;
		goto err_free;
	}

	for (i = 0; i < nxfers; i++) {
		bool is_read = (mode == I3C_XFER_MODE_SDR) ?
			       (xfers[i].rnw == I3C_DEV_DIR_READ) :
			       (xfers[i].cmd & 0x80);

		fprintf(stdout, "Success on message %d\n", i);
		if (is_read)
			print_rx_data(&xfers[i]);
	}

	ret = EXIT_SUCCESS;

err_free:
	for (i = 0; i < nxfers; i++)
		free((void *)(uintptr_t)xfers[i].data);
	free(xfers);
	close(file);

	return ret;
}
