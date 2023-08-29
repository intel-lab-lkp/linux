// SPDX-License-Identifier: GPL-2.0-only
/* Counter - Test various watch events in a userspace application
 * inspired by counter_example.c
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/counter.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static struct counter_watch simple_watch[] = {
	{
		/* Component data: Count 0 count */
		.component.type = COUNTER_COMPONENT_COUNT,
		.component.scope = COUNTER_SCOPE_COUNT,
		.component.parent = 0,
		/* Event type: Index */
		.event = COUNTER_EVENT_OVERFLOW_UNDERFLOW,
		/* Device event channel 0 */
		.channel = 0,
	},
};

static int find_match_or_number_from_array(char *arg, const char * const str[], int sz, __u8 *val)
{
	unsigned int i;
	char *dummy;
	unsigned long idx;
	int ret;

	for (i = 0; i < sz; i++) {
		ret = strncmp(arg, str[i], strlen(str[i]));
		if (!ret && strlen(str[i]) == strlen(arg)) {
			*val = i;
			return 0;
		}
	}

	/* fallback to number */
	idx = strtoul(optarg, &dummy, 10);
	if (!errno) {
		if (idx >= sz)
			return -EINVAL;
		*val = idx;
		return 0;
	}

	return -errno;
}

static const char * const counter_event_type_name[] = {
	"COUNTER_EVENT_OVERFLOW",
	"COUNTER_EVENT_UNDERFLOW",
	"COUNTER_EVENT_OVERFLOW_UNDERFLOW",
	"COUNTER_EVENT_THRESHOLD",
	"COUNTER_EVENT_INDEX",
	"COUNTER_EVENT_CHANGE_OF_STATE",
	"COUNTER_EVENT_CAPTURE",
};

static int counter_arg_to_event_type(char *arg, __u8 *event)
{
	return find_match_or_number_from_array(arg, counter_event_type_name,
					       ARRAY_SIZE(counter_event_type_name), event);
}

static const char * const counter_component_type_name[] = {
	"COUNTER_COMPONENT_NONE",
	"COUNTER_COMPONENT_SIGNAL",
	"COUNTER_COMPONENT_COUNT",
	"COUNTER_COMPONENT_FUNCTION",
	"COUNTER_COMPONENT_SYNAPSE_ACTION",
	"COUNTER_COMPONENT_EXTENSION",
};

static int counter_arg_to_component_type(char *arg, __u8 *type)
{
	return find_match_or_number_from_array(arg, counter_component_type_name,
					       ARRAY_SIZE(counter_component_type_name), type);
}

static const char * const counter_scope_name[] = {
	"COUNTER_SCOPE_DEVICE",
	"COUNTER_SCOPE_SIGNAL",
	"COUNTER_SCOPE_COUNT",
};

static int counter_arg_to_scope(char *arg, __u8 *type)
{
	return find_match_or_number_from_array(arg, counter_scope_name,
					       ARRAY_SIZE(counter_scope_name), type);
}

static void print_usage(void)
{
	fprintf(stderr, "Usage: counter_watch_events [options]...\n"
		"Test various watch events for given counter device\n"
		"  --channel -c <n>\n"
		"        Set watch.channel\n"
		"  --debug -d\n"
		"        Prints debug information\n"
		"  --event -e <number or counter_event_type string>\n"
		"        Sets watch.event\n"
		"  --help -h\n"
		"        Prints usage\n"
		"  --device-num -n <n>\n"
		"        Set device number (/dev/counter<n>, default to 0)\n"
		"  --id -i <n>\n"
		"        Set watch.component.id\n"
		"  --loop -l <n>\n"
		"        Loop for a number of events (forever if n < 0)\n"
		"  --parent -p <n>\n"
		"        Set watch.component.parent number\n"
		"  --scope -s <number or counter_scope string>\n"
		"        Set watch.component.scope\n"
		"  --type -t <number or counter_component_type string>\n"
		"        Set watch.component.type\n"
		"\n"
		"Example with two watched events:\n\n"
		"counter_watch_events -d \\\n"
		"\t-t COUNTER_COMPONENT_COUNT -s COUNTER_SCOPE_COUNT"
		" -e COUNTER_EVENT_OVERFLOW_UNDERFLOW -i 0 -c 0 \\\n"
		"\t-t COUNTER_COMPONENT_EXTENSION -s COUNTER_SCOPE_COUNT"
		" -e COUNTER_EVENT_CAPTURE -i 7 -c 3\n"
		);
}

static void print_watch(struct counter_watch *watch, int nwatch)
{
	int i;

	/* prints the watch array in C-like structure */
	printf("watch[%d] = {\n", nwatch);
	for (i = 0; i < nwatch; i++) {
		printf(" [%d] =\t{\n"
		       "\t\t.component.type = %s\n"
		       "\t\t.component.scope = %s\n"
		       "\t\t.component.parent = %d\n"
		       "\t\t.component.id = %d\n"
		       "\t\t.event = %s\n"
		       "\t\t.channel = %d\n"
		       "\t},\n",
		       i,
		       counter_component_type_name[watch[i].component.type],
		       counter_scope_name[watch[i].component.scope],
		       watch[i].component.parent,
		       watch[i].component.id,
		       counter_event_type_name[watch[i].event],
		       watch[i].channel);
	}
	printf("};\n");
}

static const struct option longopts[] = {
	{ "channel",		required_argument, 0, 'c' },
	{ "debug",		no_argument,       0, 'd' },
	{ "event",		required_argument, 0, 'e' },
	{ "help",		no_argument,       0, 'h' },
	{ "device-num",		required_argument, 0, 'n' },
	{ "id",			required_argument, 0, 'i' },
	{ "loop",		required_argument, 0, 'l' },
	{ "parent",		required_argument, 0, 'p' },
	{ "scope",		required_argument, 0, 's' },
	{ "type",		required_argument, 0, 't' },
	{ },
};

int main(int argc, char **argv)
{
	int c, fd, i, ret;
	struct counter_event event_data;
	char *device_name = NULL;
	int debug = 0, loop = -1;
	char *dummy;
	int dev_num = 0, nwatch = 0, ncfg[] = {0, 0, 0, 0, 0, 0};
	int num_chan = 0, num_evt = 0, num_id = 0, num_p = 0, num_s = 0, num_t = 0;
	struct counter_watch *watches;

	/*
	 * 1st pass: count events configurations number to allocate the watch array.
	 * Each watch argument can be repeated several times: each time it gets repeated,
	 * a corresponding watch is allocated (and configured) in 2nd pass.
	 */
	while ((c = getopt_long(argc, argv, "c:de:hn:i:l:p:s:t:", longopts, NULL)) != -1) {
		switch (c) {
		case 'c':
			ncfg[0]++;
			break;
		case 'e':
			ncfg[1]++;
			break;
		case 'i':
			ncfg[2]++;
			break;
		case 'p':
			ncfg[3]++;
			break;
		case 's':
			ncfg[4]++;
			break;
		case 't':
			ncfg[5]++;
			break;
		};
	};

	for (i = 0; i < ARRAY_SIZE(ncfg); i++)
		if (ncfg[i] > nwatch)
			nwatch = ncfg[i];

	if (nwatch) {
		watches = calloc(nwatch, sizeof(*watches));
	} else {
		/* default to simple watch example */
		watches = simple_watch;
		nwatch = ARRAY_SIZE(simple_watch);
	}

	/* 2nd pass: read arguments to fill in watch array */
	optind = 1;
	while ((c = getopt_long(argc, argv, "c:de:hn:i:l:p:s:t:", longopts, NULL)) != -1) {
		switch (c) {
		case 'c':
			/* watch.channel */
			watches[num_chan].channel = strtoul(optarg, &dummy, 10);
			if (errno)
				return -errno;
			num_chan++;
			break;
		case 'd':
			debug = 1;
			break;
		case 'e':
			/* watch.event */
			ret = counter_arg_to_event_type(optarg, &watches[num_evt].event);
			if (ret)
				return ret;
			num_evt++;
			break;
		case 'h':
			print_usage();
			return -1;
		case 'n':
			errno = 0;
			dev_num = strtoul(optarg, &dummy, 10);
			if (errno)
				return -errno;
			break;
		case 'i':
			/* watch.component.id */
			watches[num_id].component.id = strtoul(optarg, &dummy, 10);
			if (errno)
				return -errno;
			num_id++;
			break;
		case 'l':
			loop = strtol(optarg, &dummy, 10);
			if (errno)
				return -errno;
			break;
		case 'p':
			/* watch.component.parent */
			watches[num_p].component.parent = strtoul(optarg, &dummy, 10);
			if (errno)
				return -errno;
			num_p++;
			break;
		case 's':
			/* watch.component.scope */
			ret = counter_arg_to_scope(optarg, &watches[num_s].component.scope);
			if (ret)
				return ret;
			num_s++;
			break;
		case 't':
			/* watch.component.type */
			ret = counter_arg_to_component_type(optarg, &watches[num_t].component.type);
			if (ret)
				return ret;
			num_t++;
			break;
		}

	};

	if (debug)
		print_watch(watches, nwatch);

	ret = asprintf(&device_name, "/dev/counter%d", dev_num);
	if (ret < 0)
		return -ENOMEM;

	if (debug)
		printf("Opening %s\n", device_name);

	fd = open(device_name, O_RDWR);
	if (fd == -1) {
		perror("Unable to open counter device");
		return 1;
	}

	for (i = 0; i < nwatch; i++) {
		ret = ioctl(fd, COUNTER_ADD_WATCH_IOCTL, watches + i);
		if (ret == -1) {
			fprintf(stderr, "Error adding watches[%d]: %s\n", i,
				strerror(errno));
			return 1;
		}
	}

	ret = ioctl(fd, COUNTER_ENABLE_EVENTS_IOCTL);
	if (ret == -1) {
		perror("Error enabling events");
		return 1;
	}

	for (i = 0; loop <= 0 || i < loop; i++) {
		ret = read(fd, &event_data, sizeof(event_data));
		if (ret == -1) {
			perror("Failed to read event data");
			return 1;
		}

		if (ret != sizeof(event_data)) {
			fprintf(stderr, "Failed to read event data\n");
			return -EIO;
		}

		printf("Timestamp: %llu\tData: %llu\t event: %s\tch: %d\n",
		       event_data.timestamp, event_data.value,
		       counter_event_type_name[event_data.watch.event],
		       event_data.watch.channel);

		if (event_data.status) {
			fprintf(stderr, "Error %d: %s\n", event_data.status,
				strerror(event_data.status));
		}
	}

	return 0;
}
