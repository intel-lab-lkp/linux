// SPDX-License-Identifier: GPL-2.0
/*
* Watchdog Driver Test Program
* - Tests all ioctls
* - Tests Magic Close - CONFIG_WATCHDOG_NOWAYOUT
* - Could be tested against softdog driver on systems that
*   don't have watchdog hardware.
* - TODO:
* - Enhance test to add coverage for WDIOC_GETTEMP.
*
* Reference: Documentation/watchdog/watchdog-api.rst
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/watchdog.h>
#include "../kselftest.h"

#define DEFAULT_PING_RATE	1
#define DEFAULT_PING_COUNT	5

int fd;
const char v = 'V';
static const char sopts[] = "bdehp:c:st:Tn:NLf:i";
static const char topts[] = "bdeLn:Nst:T";
static const struct option lopts[] = {
	{"bootstatus",          no_argument, NULL, 'b'},
	{"disable",             no_argument, NULL, 'd'},
	{"enable",              no_argument, NULL, 'e'},
	{"help",                no_argument, NULL, 'h'},
	{"pingrate",      required_argument, NULL, 'p'},
	{"pingcount",     required_argument, NULL, 'c'},
	{"status",              no_argument, NULL, 's'},
	{"timeout",       required_argument, NULL, 't'},
	{"gettimeout",          no_argument, NULL, 'T'},
	{"pretimeout",    required_argument, NULL, 'n'},
	{"getpretimeout",       no_argument, NULL, 'N'},
	{"gettimeleft",		no_argument, NULL, 'L'},
	{"file",          required_argument, NULL, 'f'},
	{"info",		no_argument, NULL, 'i'},
	{NULL,                  no_argument, NULL, 0x0}
};

/*
 * This function simply sends an IOCTL to the driver, which in turn ticks
 * the PC Watchdog card to reset its internal timer so it doesn't trigger
 * a computer reset.
 */
static int keep_alive(void)
{
	int dummy;
	int ret;

	ret = ioctl(fd, WDIOC_KEEPALIVE, &dummy);
	if (!ret)
		printf(".");

	return ret;
}

/*
 * The main program.  Run the program with "-d" to disable the card,
 * or "-e" to enable the card.
 */

static void term(int sig)
{
	int ret = write(fd, &v, 1);

	close(fd);
	if (ret < 0)
		ksft_print_msg("\nStopping watchdog ticks failed (%d)...\n", errno);
	else
		ksft_print_msg("\nStopping watchdog ticks...\n");
	exit(0);
}

static void usage(char *progname)
{
	ksft_print_msg("Usage: %s [options]\n", progname);
	ksft_print_msg(" -f, --file\t\tOpen watchdog device file\n");
	ksft_print_msg("\t\t\tDefault is /dev/watchdog\n");
	ksft_print_msg(" -i, --info\t\tShow watchdog_info\n");
	ksft_print_msg(" -s, --status\t\tGet status & supported features\n");
	ksft_print_msg(" -b, --bootstatus\tGet last boot status (Watchdog/POR)\n");
	ksft_print_msg(" -d, --disable\t\tTurn off the watchdog timer\n");
	ksft_print_msg(" -e, --enable\t\tTurn on the watchdog timer\n");
	ksft_print_msg(" -h, --help\t\tPrint the help message\n");
	ksft_print_msg(" -p, --pingrate=P\tSet ping rate to P seconds (default %d)\n",
	       DEFAULT_PING_RATE);
	ksft_print_msg(" -c, --pingcount=C\tSet number of pings to C (default %d)\n",
	       DEFAULT_PING_COUNT);
	ksft_print_msg(" -t, --timeout=T\tSet timeout to T seconds\n");
	ksft_print_msg(" -T, --gettimeout\tGet the timeout\n");
	ksft_print_msg(" -n, --pretimeout=T\tSet the pretimeout to T seconds\n");
	ksft_print_msg(" -N, --getpretimeout\tGet the pretimeout\n");
	ksft_print_msg(" -L, --gettimeleft\tGet the time left until timer expires\n");
	ksft_print_msg("\n");
	ksft_print_msg("Parameters are parsed left-to-right in real-time.\n");
	ksft_print_msg("Example: %s -d -t 10 -p 5 -e\n", progname);
	ksft_print_msg("Example: %s -t 12 -T -n 7 -N\n", progname);
}

struct wdiof_status {
	int flag;
	const char *status_str;
};

#define WDIOF_NUM_STATUS 8

static const struct wdiof_status wdiof_status[WDIOF_NUM_STATUS] = {
	{WDIOF_SETTIMEOUT,  "Set timeout (in seconds)"},
	{WDIOF_MAGICCLOSE,  "Supports magic close char"},
	{WDIOF_PRETIMEOUT,  "Pretimeout (in seconds), get/set"},
	{WDIOF_ALARMONLY,  "Watchdog triggers a management or other external alarm not a reboot"},
	{WDIOF_KEEPALIVEPING,  "Keep alive ping reply"},
	{WDIOS_DISABLECARD,  "Turn off the watchdog timer"},
	{WDIOS_ENABLECARD,  "Turn on the watchdog timer"},
	{WDIOS_TEMPPANIC,  "Kernel panic on temperature trip"},
};

static void print_status(int flags)
{
	int wdiof = 0;

	if (flags == WDIOS_UNKNOWN) {
		ksft_print_msg("Unknown status error from WDIOC_GETSTATUS\n");
		return;
	}

	for (wdiof = 0; wdiof < WDIOF_NUM_STATUS; wdiof++) {
		if (flags & wdiof_status[wdiof].flag)
			ksft_print_msg("Support/Status: %s\n",
				wdiof_status[wdiof].status_str);
	}
}

#define WDIOF_NUM_BOOTSTATUS 7

static const struct wdiof_status wdiof_bootstatus[WDIOF_NUM_BOOTSTATUS] = {
	{WDIOF_OVERHEAT, "Reset due to CPU overheat"},
	{WDIOF_FANFAULT, "Fan failed"},
	{WDIOF_EXTERN1, "External relay 1"},
	{WDIOF_EXTERN2, "External relay 2"},
	{WDIOF_POWERUNDER, "Power bad/power fault"},
	{WDIOF_CARDRESET, "Card previously reset the CPU"},
	{WDIOF_POWEROVER,  "Power over voltage"},
};

static void print_boot_status(int flags)
{
	int wdiof = 0;

	if (flags == WDIOF_UNKNOWN) {
		ksft_print_msg("Unknown flag error from WDIOC_GETBOOTSTATUS\n");
		return;
	}

	if (flags == 0) {
		ksft_print_msg("Last boot is caused by: Power-On-Reset\n");
		return;
	}

	for (wdiof = 0; wdiof < WDIOF_NUM_BOOTSTATUS; wdiof++) {
		if (flags & wdiof_bootstatus[wdiof].flag)
			ksft_print_msg("Last boot is caused by: %s\n",
				wdiof_bootstatus[wdiof].status_str);
	}
}

int main(int argc, char *argv[])
{
	int flags;
	unsigned int ping_rate = DEFAULT_PING_RATE;
	unsigned int ping_count = DEFAULT_PING_COUNT;
	int ret;
	int c;
	int oneshot = 0;
	char *file = "/dev/watchdog";
	struct watchdog_info info;
	int temperature;
	/* run WDIOC_KEEPALIVE test by default */
	int test_num = 1;

	setbuf(stdout, NULL);

	while ((c = getopt_long(argc, argv, sopts, lopts, NULL)) != -1) {
		if (c == 'f')
			file = optarg;

		if (strchr(topts, c))
			test_num++;
	}

	fd = open(file, O_WRONLY);

	if (fd == -1) {
		if (errno == ENOENT)
			ksft_exit_skip("Watchdog device (%s) not found.\n", file);
		else if (errno == EACCES)
			ksft_exit_skip("Run watchdog as root.\n");
		else
			ksft_exit_skip("Watchdog device open failed %s\n", strerror(errno));
	}

	/*
	 * Validate that `file` is a watchdog device
	 */
	ret = ioctl(fd, WDIOC_GETSUPPORT, &info);
	if (ret) {
		close(fd);
		ksft_exit_skip("WDIOC_GETSUPPORT error '%s'\n", strerror(errno));
	}

	optind = 0;

	ksft_print_header();
	ksft_set_plan(test_num);

	while ((c = getopt_long(argc, argv, sopts, lopts, NULL)) != -1) {
		switch (c) {
		case 'b':
			flags = 0;
			oneshot = 1;
			ret = ioctl(fd, WDIOC_GETBOOTSTATUS, &flags);
			if (!ret)
				print_boot_status(flags);
			else
				ksft_print_msg("WDIOC_GETBOOTSTATUS error '%s'\n", strerror(errno));
			ksft_test_result(!ret, "WDIOC_GETBOOTSTATUS\n");
			break;
		case 'd':
			flags = WDIOS_DISABLECARD;
			ret = ioctl(fd, WDIOC_SETOPTIONS, &flags);
			if (!ret)
				ksft_print_msg("Watchdog card disabled.\n");
			else {
				ksft_print_msg("WDIOS_DISABLECARD error '%s'\n", strerror(errno));
				oneshot = 1;
			}
			ksft_test_result(!ret, "WDIOC_SETOPTIONS_WDIOS_DISABLECARD\n");
			break;
		case 'e':
			flags = WDIOS_ENABLECARD;
			ret = ioctl(fd, WDIOC_SETOPTIONS, &flags);
			if (!ret)
				ksft_print_msg("Watchdog card enabled.\n");
			else {
				ksft_print_msg("WDIOS_ENABLECARD error '%s'\n", strerror(errno));
				oneshot = 1;
			}
			ksft_test_result(!ret, "WDIOC_SETOPTIONS_WDIOS_ENABLECARD\n");
			break;
		case 'p':
			ping_rate = strtoul(optarg, NULL, 0);
			if (!ping_rate)
				ping_rate = DEFAULT_PING_RATE;
			ksft_print_msg("Watchdog ping rate set to %u seconds.\n", ping_rate);
			break;
		case 'c':
			ping_count = strtoul(optarg, NULL, 0);
			if (!ping_count)
				ping_count = DEFAULT_PING_COUNT;
			ksft_print_msg("Number of pings set to %u.\n", ping_count);
			break;
		case 's':
			flags = 0;
			oneshot = 1;
			ret = ioctl(fd, WDIOC_GETSTATUS, &flags);
			if (!ret)
				print_status(flags);
			else
				ksft_print_msg("WDIOC_GETSTATUS error '%s'\n", strerror(errno));
			ksft_test_result(!ret, "WDIOC_GETSTATUS\n");
			ret = ioctl(fd, WDIOC_GETTEMP, &temperature);
			if (ret)
				ksft_print_msg("WDIOC_GETTEMP: '%s'\n", strerror(errno));
			else
				ksft_print_msg("Temperature %d\n", temperature);
			break;
		case 't':
			flags = strtoul(optarg, NULL, 0);
			ret = ioctl(fd, WDIOC_SETTIMEOUT, &flags);
			if (!ret)
				ksft_print_msg("Watchdog timeout set to %u seconds.\n", flags);
			else {
				ksft_print_msg("WDIOC_SETTIMEOUT error '%s'\n", strerror(errno));
				oneshot = 1;
			}
			ksft_test_result(!ret, "WDIOC_SETTIMEOUT\n");
			break;
		case 'T':
			oneshot = 1;
			ret = ioctl(fd, WDIOC_GETTIMEOUT, &flags);
			if (!ret)
				ksft_print_msg("WDIOC_GETTIMEOUT returns %u seconds.\n", flags);
			else
				ksft_print_msg("WDIOC_GETTIMEOUT error '%s'\n", strerror(errno));
			ksft_test_result(!ret, "WDIOC_GETTIMEOUT\n");
			break;
		case 'n':
			flags = strtoul(optarg, NULL, 0);
			ret = ioctl(fd, WDIOC_SETPRETIMEOUT, &flags);
			if (!ret)
				ksft_print_msg("Watchdog pretimeout set to %u seconds.\n", flags);
			else {
				ksft_print_msg("WDIOC_SETPRETIMEOUT error '%s'\n", strerror(errno));
				oneshot = 1;
			}
			ksft_test_result(!ret, "WDIOC_SETPRETIMEOUT\n");
			break;
		case 'N':
			oneshot = 1;
			ret = ioctl(fd, WDIOC_GETPRETIMEOUT, &flags);
			if (!ret)
				ksft_print_msg("WDIOC_GETPRETIMEOUT returns %u seconds.\n", flags);
			else
				ksft_print_msg("WDIOC_GETPRETIMEOUT error '%s'\n", strerror(errno));
			ksft_test_result(!ret, "WDIOC_GETPRETIMEOUT\n");
			break;
		case 'L':
			oneshot = 1;
			ret = ioctl(fd, WDIOC_GETTIMELEFT, &flags);
			if (!ret)
				ksft_print_msg("WDIOC_GETTIMELEFT returns %u seconds.\n", flags);
			else
				ksft_print_msg("WDIOC_GETTIMELEFT error '%s'\n", strerror(errno));
			ksft_test_result(!ret, "WDIOC_GETTIMELEFT\n");
			break;
		case 'f':
			/* Handled above */
			break;
		case 'i':
			/*
			 * watchdog_info was obtained as part of file open
			 * validation. So we just show it here.
			 */
			oneshot = 1;
			ksft_print_msg("watchdog_info:\n");
			ksft_print_msg(" identity:\t\t%s\n", info.identity);
			ksft_print_msg(" firmware_version:\t%u\n", info.firmware_version);
			print_status(info.options);
			break;

		default:
			usage(argv[0]);
			ksft_test_result_skip("WDIOC_KEEPALIVE\n");
			goto end;
		}
	}

	if (oneshot) {
		ksft_test_result_skip("WDIOC_KEEPALIVE\n");
		goto end;
	}

	ksft_print_msg("Watchdog Ticking Away!\n");
	ksft_print_msg("");

	signal(SIGINT, term);

	while (ping_count > 0) {
		ret = keep_alive();
		if (ret)
			break;
		sleep(ping_rate);
		ping_count--;
	}
	printf("\n");
	ksft_test_result(!ret, "WDIOC_KEEPALIVE\n");
end:
	/*
	 * Send specific magic character 'V' just in case Magic Close is
	 * enabled to ensure watchdog gets disabled on close.
	 */
	ret = write(fd, &v, 1);
	if (ret < 0)
		ksft_print_msg("Stopping watchdog ticks failed (%d)...\n", errno);
	close(fd);
	ksft_finished();
}
