// SPDX-License-Identifier: GPL-2.0
/*
 * sdio-raw - raw SDIO register access via the SDIO userspace
 * passthrough character device (CONFIG_MMC_SDIO_CDEV).
 *
 * Reference/debug client for the SDIO_IOC_RW (CMD52) and
 * SDIO_IOC_RW_EXT (CMD53) ioctls exposed by /dev/mmcX:YYYY:FN.
 *
 * The ioctls require CAP_SYS_ADMIN; raw access while a function
 * driver is bound can disturb the state the driver maintains.
 *
 * Author: Shawn Lin <shawn.lin@linux.dev>
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <linux/mmc/sdio_ioctl.h>

#ifdef __has_include
#if __has_include("sdio_ids_gen.h")
#include "sdio_ids_gen.h"
#define HAVE_SDIO_IDS	1
#endif
#endif

#ifndef HAVE_SDIO_IDS
/*
 * No generated tables (e.g. manual build without the tools Makefile):
 * everything is reported as unlisted.
 */
struct sdio_id_name {
	unsigned int id;
	const char *name;
};
static const struct sdio_id_name sdio_class_names[] = { { ~0U, "" } };
static const struct sdio_id_name sdio_device_names[] = { { ~0U, "" } };
#endif

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

static const char *progname = "sdio-raw";

static _Noreturn void usage(int rc)
{
	fprintf(rc ? stderr : stdout,
"Usage: %s -d DEV COMMAND [ARGS]\n"
"\n"
"Raw SDIO register access through the SDIO passthrough character\n"
"device. The ioctls require CAP_SYS_ADMIN (run as root).\n"
"\n"
"Options:\n"
"  -d, --device DEV   SDIO function chardev, e.g. /dev/mmc1:0001:1\n"
"  -f, --func FN      override function number (default: parsed from DEV)\n"
"  -h, --help         show this help\n"
"\n"
"Commands:\n"
"  cmd52 -a ADDR [-w VAL] [-r]      read one byte (-w: write it, -r: read back)\n"
"  cmd53 -a ADDR -n COUNT [-w FILE] [-o FILE] [--fixed]\n"
"                                   byte mode read/write, 1..512 bytes\n"
"  list                             enumerate SDIO functions (sysfs)\n"
"  cccr                             dump CCCR (function 0)\n"
"  fbr                              dump FBR of the selected function\n"
"  cis [-f FN]                      walk the common (fn 0) or function CIS\n"
"\n"
"Addresses accept 0x-hex or decimal. cmd53 read goes to stdout as a\n"
"hexdump unless -o is given; cmd53 write reads from FILE ('-' = stdin).\n",
		progname);
	exit(rc);
}

static void die_errno(const char *what)
{
	fprintf(stderr, "%s: %s: %s", progname, what, strerror(errno));
	if (errno == EPERM)
		fprintf(stderr, " (CAP_SYS_ADMIN required, run as root)");
	fputc('\n', stderr);
	exit(1);
}

static unsigned long parse_num(const char *s, const char *what)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || end == s || *end) {
		fprintf(stderr, "%s: invalid %s: %s\n", progname, what, s);
		exit(1);
	}
	return v;
}

/* /dev/mmc1:0001:1 -> function 1 */
static unsigned int fn_from_dev(const char *dev)
{
	const char *p = strrchr(dev, ':');

	if (p && p[1])
		return strtoul(p + 1, NULL, 10);
	return 0;
}

static int open_dev(const char *dev)
{
	int fd;

	if (!dev) {
		fprintf(stderr, "%s: no device given, use -d DEV\n", progname);
		exit(1);
	}

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s: open %s: %s\n", progname, dev,
			strerror(errno));
		exit(1);
	}
	return fd;
}

static int rd52(int fd, unsigned int fn, unsigned int addr,
		unsigned char *val)
{
	struct sdio_ioc_rw rw = {
		.fn = fn,
		.addr = addr,
	};

	if (ioctl(fd, SDIO_IOC_RW, &rw) < 0)
		return -1;
	*val = rw.data;
	return 0;
}

static void hexdump(const unsigned char *buf, unsigned int len)
{
	unsigned int i, j;

	for (i = 0; i < len; i += 16) {
		printf("  %04x:", i);
		for (j = 0; j < 16; j++) {
			if (i + j < len)
				printf(" %02x", buf[i + j]);
			else
				printf("   ");
		}
		printf("  ");
		for (j = 0; j < 16 && i + j < len; j++)
			putchar(isprint(buf[i + j]) ? buf[i + j] : '.');
		putchar('\n');
	}
}

static void cmd52(int fd, unsigned int fn, unsigned long addr, int write,
		  unsigned long val, int raw)
{
	struct sdio_ioc_rw rw = {
		.write = write,
		.fn = fn,
		.raw = raw,
		.addr = addr,
		.data = val,
	};

	if (ioctl(fd, SDIO_IOC_RW, &rw) < 0)
		die_errno("CMD52 failed");

	if (write && !raw)
		printf("fn%u addr 0x%05lx: wrote 0x%02lx\n", rw.fn, addr, val);
	else
		printf("fn%u addr 0x%05lx: 0x%02x\n", rw.fn, addr, rw.data);
}

static void cmd53(int fd, unsigned int fn, unsigned long addr,
		  unsigned long count,
		  int write, int incr_addr, const char *infile,
		  const char *outfile)
{
	struct sdio_ioc_rw_ext rw;
	FILE *f;

	memset(&rw, 0, sizeof(rw));
	rw.write = write;
	rw.fn = fn;
	rw.incr_addr = incr_addr;
	rw.addr = addr;
	rw.count = count;

	if (write) {
		f = (!infile || !strcmp(infile, "-")) ? stdin :
							fopen(infile, "rb");
		if (!f) {
			fprintf(stderr, "%s: open %s: %s\n", progname, infile,
				strerror(errno));
			exit(1);
		}
		if (fread(rw.data, 1, count, f) != count) {
			fprintf(stderr, "%s: short read from %s\n", progname,
				infile ? infile : "stdin");
			exit(1);
		}
		if (f != stdin)
			fclose(f);
	}

	if (ioctl(fd, SDIO_IOC_RW_EXT, &rw) < 0)
		die_errno("CMD53 failed");

	if (write)
		return;

	if (outfile) {
		f = fopen(outfile, "wb");
		if (!f) {
			fprintf(stderr, "%s: open %s: %s\n", progname, outfile,
				strerror(errno));
			exit(1);
		}
		fwrite(rw.data, 1, count, f);
		fclose(f);
	} else {
		hexdump(rw.data, count);
	}
}

static void cccr(int fd)
{
	static const struct {
		unsigned char addr;
		unsigned char len;
		const char *name;
	} fields[] = {
		{ 0x00, 1, "CCCR/SDIO revision" },
		{ 0x01, 1, "SD spec revision" },
		{ 0x02, 1, "I/O enable" },
		{ 0x03, 1, "I/O ready" },
		{ 0x04, 1, "Int enable" },
		{ 0x05, 1, "Int pending" },
		{ 0x06, 1, "I/O abort" },
		{ 0x07, 1, "Bus interface control" },
		{ 0x08, 1, "Card capability" },
		{ 0x09, 3, "Common CIS pointer" },
		{ 0x0c, 1, "Bus suspend" },
		{ 0x0d, 1, "Function select" },
		{ 0x0e, 1, "Exec flags" },
		{ 0x0f, 1, "Ready flags" },
		{ 0x10, 2, "FN0 block size" },
		{ 0x12, 1, "Power control" },
		{ 0x13, 1, "High-speed" },
	};
	unsigned char regs[0x18];
	unsigned int i, j;
	unsigned long v;

	for (i = 0; i < sizeof(regs); i++)
		if (rd52(fd, 0, i, &regs[i]))
			die_errno("CMD52 (CCCR dump) failed");

	printf("CCCR (fn 0):\n");
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		v = 0;
		for (j = 0; j < fields[i].len; j++)
			v |= (unsigned long)regs[fields[i].addr + j] << (8 * j);

		if (fields[i].len == 1)
			printf("  0x%02x       %-24s 0x%02lx\n", fields[i].addr,
			       fields[i].name, v);
		else if (fields[i].len == 2)
			printf("  0x%02x-0x%02x %-21s 0x%04lx\n",
			       fields[i].addr, fields[i].addr + 1,
			       fields[i].name, v);
		else
			printf("  0x%02x-0x%02x %-21s 0x%06lx\n",
			       fields[i].addr,
			       fields[i].addr + fields[i].len - 1,
			       fields[i].name, v);
	}
	printf("  raw 0x00-0x17:\n");
	hexdump(regs, sizeof(regs));
}

static void fbr(int fd, unsigned int fn)
{
	unsigned char regs[0x20];
	unsigned long cis, csa;
	unsigned int i;

	if (fn == 0) {
		fprintf(stderr, "%s: %s needs a function 1..7, use -f\n",
			progname, __func__);
		exit(1);
	}

	/*
	 * The FBR of function fn lives at fn * 0x100 in the common
	 * register area and is always accessed through function 0, like
	 * the kernel does (SDIO_FBR_BASE(), drivers/mmc/core/sdio_cis.c).
	 */
	for (i = 0; i < sizeof(regs); i++)
		if (rd52(fd, 0, fn * 0x100 + i, &regs[i]))
			die_errno("CMD52 (FBR dump) failed");

	cis = regs[0x09] | regs[0x0a] << 8 | regs[0x0b] << 16;
	csa = regs[0x0c] | regs[0x0d] << 8 | regs[0x0e] << 16;

	printf("FBR (fn %u) @ 0x%03x:\n", fn, fn * 0x100);
	printf("  0x00      Standard interface code 0x%02x\n", regs[0x00]);
	printf("  0x01      Ext std interface code  0x%02x\n", regs[0x01]);
	printf("  0x02      Power selection         0x%02x\n", regs[0x02]);
	printf("  0x09-0x0b CIS pointer               0x%06lx\n", cis);
	printf("  0x0c-0x0e CSA pointer               0x%06lx\n", csa);
	printf("  0x10-0x11 Function block size       0x%04x\n",
	       regs[0x10] | regs[0x11] << 8);
	printf("  raw 0x00-0x1f:\n");
	hexdump(regs, sizeof(regs));
}

/*
 * CIS walk, mirroring drivers/mmc/core/sdio_cis.c: the CIS pointer
 * sits at offset 0x09 of the CCCR (fn 0) or of the FBR of the selected
 * function, and both the pointer and the tuple chain are read through
 * function 0.  Function 0 dumps the common CIS, functions 1..7 their
 * per-function CIS.
 */
#define CISTPL_END	0xff
#define CISTPL_VERS_1	0x15
#define CISTPL_MANFID	0x20
#define CISTPL_FUNCID	0x21

static const char *tpl_name(unsigned char code)
{
	switch (code) {
	case CISTPL_VERS_1:	return "VERS_1";
	case CISTPL_MANFID:	return "MANFID";
	case CISTPL_FUNCID:	return "FUNCID";
	default:		return "";
	}
}

static void cis_dump(int fd, unsigned int fn)
{
	unsigned char b[3], body[256];
	unsigned long addr;
	unsigned int n;

	if (rd52(fd, 0, fn * 0x100 + 0x09, &b[0]) ||
	    rd52(fd, 0, fn * 0x100 + 0x0a, &b[1]) ||
	    rd52(fd, 0, fn * 0x100 + 0x0b, &b[2]))
		die_errno("CMD52 (CIS pointer) failed");

	addr = b[0] | b[1] << 8 | b[2] << 16;
	if (!addr) {
		printf("CIS (fn %u): not present (CIS pointer is 0)\n", fn);
		return;
	}
	addr &= 0x1ffff;

	printf("CIS (fn %u) @ 0x%05lx:\n", fn, addr);

	for (n = 0; n < 256; n++) {
		unsigned char code, link;
		unsigned int i;

		if (rd52(fd, 0, addr, &code))
			die_errno("CMD52 (CIS walk) failed");
		if (code == CISTPL_END) {
			printf("  0x%05lx TPL 0xff END\n", addr);
			break;
		}
		if (code == 0x00) {	/* null tuple, has no link field */
			printf("  0x%05lx TPL 0x00 NULL\n", addr);
			addr++;
			continue;
		}
		if (rd52(fd, 0, addr + 1, &link))
			die_errno("CMD52 (CIS walk) failed");
		if (link == 0xff) {
			printf("  0x%05lx TPL 0x%02x link=0xff (undefined), stop\n",
			       addr, code);
			break;
		}

		for (i = 0; i < link; i++)
			if (rd52(fd, 0, addr + 2 + i, &body[i]))
				die_errno("CMD52 (CIS walk) failed");

		printf("  0x%05lx TPL 0x%02x %-6s len=%u\n", addr, code,
		       tpl_name(code), link);

		switch (code) {
		case CISTPL_MANFID:
			if (link >= 4) {
				printf("    manufacturer 0x%04x, product 0x%04x\n",
				       body[0] | body[1] << 8,
				       body[2] | body[3] << 8);
				break;
			}
			hexdump(body, link);
			break;
		case CISTPL_VERS_1: {
			unsigned int j = 2;

			printf("    rev %u.%u:", body[0], body[1]);
			while (j < link && body[j] != 0xff) {
				unsigned int start = j;

				while (j < link && body[j])
					j++;
				printf(" \"%.*s\"", (int)(j - start),
				       (const char *)&body[start]);
				j++;	/* skip the NUL */
			}
			putchar('\n');
			break;
		}
		case CISTPL_FUNCID:
			if (link >= 1) {
				printf("    function code 0x%02x\n", body[0]);
				break;
			}
			hexdump(body, link);
			break;
		default:
			hexdump(body, link);
			break;
		}

		addr += 2 + link;
	}
}

#ifndef SDIO_SYSFS_DEVICES
#define SDIO_SYSFS_DEVICES	"/sys/bus/sdio/devices"
#endif

static int read_attr(const char *dir, const char *attr, char *buf, size_t len)
{
	char path[PATH_MAX];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "%s/%s", dir, attr);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	n = strlen(buf);
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	return 0;
}

static const char *lookup_name(const struct sdio_id_name *tbl,
			       unsigned int count, unsigned int id)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		if (tbl[i].id == id)
			return tbl[i].name;
	return NULL;
}

/*
 * Join the CIS strings the kernel parsed at enumeration (sysfs
 * info1..info4, from CISTPL_VERS_1) into one identification string.
 */
static void read_info(const char *dir, char *buf, size_t len)
{
	static const char * const attrs[] = { "info1", "info2", "info3", "info4" };
	char tmp[64];
	size_t used = 0;
	unsigned int i;

	buf[0] = '\0';
	for (i = 0; i < ARRAY_SIZE(attrs); i++) {
		size_t slen;

		if (read_attr(dir, attrs[i], tmp, sizeof(tmp)) || !tmp[0])
			continue;

		slen = strlen(tmp);
		if (used + slen + 1 >= len)
			break;
		if (used)
			buf[used++] = ' ';
		memcpy(buf + used, tmp, slen + 1);
		used += slen;
	}
}

static void list_devices(void)
{
	struct dirent **entries;
	int i, n, total = 0;

	n = scandir(SDIO_SYSFS_DEVICES, &entries, NULL, alphasort);
	if (n < 0) {
		printf("no SDIO bus (%s: %s)\n", SDIO_SYSFS_DEVICES,
		       strerror(errno));
		return;
	}

	printf("%-20s %-14s %-24s %-14s %-5s %s\n",
	       "NODE", "VID:PID", "NAME", "CLASS", "REV", "INFO");

	for (i = 0; i < n; i++) {
		const char *devname = entries[i]->d_name;
		const char *dname, *cname;
		char path[PATH_MAX];
		char vend[16], devid[16], clsv[16], rev[32], info[160];
		char namebuf[64], clsbuf[40], vidpid[16];
		unsigned int vid, pid, c;

		if (devname[0] == '.')
			continue;

		snprintf(path, sizeof(path), "%s/%s", SDIO_SYSFS_DEVICES,
			 devname);

		if (read_attr(path, "vendor", vend, sizeof(vend)) ||
		    read_attr(path, "device", devid, sizeof(devid)) ||
		    read_attr(path, "class", clsv, sizeof(clsv)) ||
		    read_attr(path, "revision", rev, sizeof(rev)))
			continue;

		read_info(path, info, sizeof(info));

		vid = strtoul(vend, NULL, 0);
		pid = strtoul(devid, NULL, 0);
		c = strtoul(clsv, NULL, 0);

		snprintf(vidpid, sizeof(vidpid), "0x%04x:0x%04x", vid, pid);

		dname = lookup_name(sdio_device_names,
				    ARRAY_SIZE(sdio_device_names), pid);
		if (!dname) {
			snprintf(namebuf, sizeof(namebuf), "Unlisted(0x%04x)",
				 pid);
			dname = namebuf;
		}

		cname = lookup_name(sdio_class_names,
				    ARRAY_SIZE(sdio_class_names), c);
		if (cname)
			snprintf(clsbuf, sizeof(clsbuf), "0x%02x (%s)", c,
				 cname);
		else
			snprintf(clsbuf, sizeof(clsbuf), "0x%02x", c);

		printf("/dev/%-15s %-14s %-24s %-14s %-5s %s\n",
		       devname, vidpid, dname, clsbuf, rev,
		       info[0] ? info : "-");
		total++;
	}

	if (!total)
		printf("no SDIO device found\n");

	for (i = 0; i < n; i++)
		free(entries[i]);
	free(entries);
}

static unsigned int get_fn(const char *dev, int fn_override)
{
	unsigned int fn = fn_override >= 0 ? (unsigned int)fn_override :
					 fn_from_dev(dev);

	if (fn > 7) {
		fprintf(stderr, "%s: invalid function number %u\n", progname,
			fn);
		exit(1);
	}
	return fn;
}

static unsigned long get_addr(const char *s)
{
	unsigned long addr = parse_num(s, "address");

	if (addr > 0x1ffff) {
		fprintf(stderr, "%s: addr 0x%lx out of range\n", progname,
			addr);
		exit(1);
	}
	return addr;
}

int main(int argc, char **argv)
{
	const char *dev = NULL, *cmd;
	const char *infile = NULL, *outfile = NULL;
	unsigned long addr = 0, count = 0, val = 0;
	int fn_override = -1, write = 0, raw = 0, incr_addr = 1;
	unsigned int fn;
	int i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "-d") || !strcmp(a, "--device")) {
			if (++i >= argc)
				usage(1);
			dev = argv[i];
		} else if (!strcmp(a, "-f") || !strcmp(a, "--func")) {
			if (++i >= argc)
				usage(1);
			fn_override = parse_num(argv[i], "function number");
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(0);
		} else {
			break;
		}
	}

	if (i >= argc)
		usage(1);

	cmd = argv[i++];

	if (!strcmp(cmd, "cmd52")) {
		for (; i < argc; i++) {
			if (!strcmp(argv[i], "-a") && i + 1 < argc)
				addr = get_addr(argv[++i]);
			else if (!strcmp(argv[i], "-w") && i + 1 < argc)
				write = 1, val = parse_num(argv[++i], "byte value");
			else if (!strcmp(argv[i], "-r"))
				raw = 1;
			else if (!strcmp(argv[i], "-f") && i + 1 < argc)
				fn_override = parse_num(argv[++i], "function number");
			else
				usage(1);
		}
		fn = get_fn(dev, fn_override);
		cmd52(open_dev(dev), fn, addr, write, val, raw);
	} else if (!strcmp(cmd, "cmd53")) {
		for (; i < argc; i++) {
			if (!strcmp(argv[i], "-a") && i + 1 < argc)
				addr = get_addr(argv[++i]);
			else if (!strcmp(argv[i], "-n") && i + 1 < argc)
				count = parse_num(argv[++i], "count");
			else if (!strcmp(argv[i], "-w") && i + 1 < argc)
				write = 1, infile = argv[++i];
			else if (!strcmp(argv[i], "-o") && i + 1 < argc)
				outfile = argv[++i];
			else if (!strcmp(argv[i], "--fixed"))
				incr_addr = 0;
			else if (!strcmp(argv[i], "-f") && i + 1 < argc)
				fn_override = parse_num(argv[++i], "function number");
			else
				usage(1);
		}
		if (!addr && !count) {
			fprintf(stderr, "%s: cmd53 needs -a ADDR and -n COUNT\n",
				progname);
			usage(1);
		}
		if (!count || count > 512) {
			fprintf(stderr, "%s: count must be 1..512\n", progname);
			exit(1);
		}
		fn = get_fn(dev, fn_override);
		if (fn == 0) {
			fprintf(stderr, "%s: cmd53 is not valid for fn 0\n",
				progname);
			exit(1);
		}
		cmd53(open_dev(dev), fn, addr, count, write, incr_addr,
		      infile, outfile);
	} else if (!strcmp(cmd, "list")) {
		list_devices();
	} else if (!strcmp(cmd, "cccr")) {
		cccr(open_dev(dev));
	} else if (!strcmp(cmd, "cis")) {
		for (; i < argc; i++) {
			if (!strcmp(argv[i], "-f") && i + 1 < argc)
				fn_override = parse_num(argv[++i], "function number");
			else
				usage(1);
		}
		fn = get_fn(dev, fn_override);
		cis_dump(open_dev(dev), fn);
	} else if (!strcmp(cmd, "fbr")) {
		for (; i < argc; i++) {
			if (!strcmp(argv[i], "-f") && i + 1 < argc)
				fn_override = parse_num(argv[++i], "function number");
			else
				usage(1);
		}
		fn = get_fn(dev, fn_override);
		fbr(open_dev(dev), fn);
	} else {
		fprintf(stderr, "%s: unknown command: %s\n", progname, cmd);
		usage(1);
	}

	return 0;
}
