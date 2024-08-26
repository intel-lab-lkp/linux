// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2022 Red Hat */

#include "../kselftest_harness.h"

#include <fcntl.h>
#include <fnmatch.h>
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <linux/hidraw.h>
#include <linux/uhid.h>

#define SHOW_UHID_DEBUG 0

#define min(a, b) \
	({ __typeof__(a) _a = (a); \
	__typeof__(b) _b = (b); \
	_a < _b ? _a : _b; })

/* for older kernels */
#ifndef HIDIOCREVOKE
#define HIDIOCREVOKE	      _IOW('H', 0x0D, int) /* Revoke device access */
#endif /* HIDIOCREVOKE */

static unsigned char rdesc[] = {
	0x06, 0x00, 0xff,	/* Usage Page (Vendor Defined Page 1) */
	0x09, 0x21,		/* Usage (Vendor Usage 0x21) */
	0xa1, 0x01,		/* COLLECTION (Application) */
	0x09, 0x01,			/* Usage (Vendor Usage 0x01) */
	0xa1, 0x00,			/* COLLECTION (Physical) */
	0x85, 0x02,				/* REPORT_ID (2) */
	0x19, 0x01,				/* USAGE_MINIMUM (1) */
	0x29, 0x08,				/* USAGE_MAXIMUM (3) */
	0x15, 0x00,				/* LOGICAL_MINIMUM (0) */
	0x25, 0xff,				/* LOGICAL_MAXIMUM (255) */
	0x95, 0x08,				/* REPORT_COUNT (8) */
	0x75, 0x08,				/* REPORT_SIZE (8) */
	0x81, 0x02,				/* INPUT (Data,Var,Abs) */
	0xc0,				/* END_COLLECTION */
	0x09, 0x01,			/* Usage (Vendor Usage 0x01) */
	0xa1, 0x00,			/* COLLECTION (Physical) */
	0x85, 0x01,				/* REPORT_ID (1) */
	0x06, 0x00, 0xff,			/* Usage Page (Vendor Defined Page 1) */
	0x19, 0x01,				/* USAGE_MINIMUM (1) */
	0x29, 0x03,				/* USAGE_MAXIMUM (3) */
	0x15, 0x00,				/* LOGICAL_MINIMUM (0) */
	0x25, 0x01,				/* LOGICAL_MAXIMUM (1) */
	0x95, 0x03,				/* REPORT_COUNT (3) */
	0x75, 0x01,				/* REPORT_SIZE (1) */
	0x81, 0x02,				/* INPUT (Data,Var,Abs) */
	0x95, 0x01,				/* REPORT_COUNT (1) */
	0x75, 0x05,				/* REPORT_SIZE (5) */
	0x81, 0x01,				/* INPUT (Cnst,Var,Abs) */
	0x05, 0x01,				/* USAGE_PAGE (Generic Desktop) */
	0x09, 0x30,				/* USAGE (X) */
	0x09, 0x31,				/* USAGE (Y) */
	0x15, 0x81,				/* LOGICAL_MINIMUM (-127) */
	0x25, 0x7f,				/* LOGICAL_MAXIMUM (127) */
	0x75, 0x10,				/* REPORT_SIZE (16) */
	0x95, 0x02,				/* REPORT_COUNT (2) */
	0x81, 0x06,				/* INPUT (Data,Var,Rel) */

	0x06, 0x00, 0xff,			/* Usage Page (Vendor Defined Page 1) */
	0x19, 0x01,				/* USAGE_MINIMUM (1) */
	0x29, 0x03,				/* USAGE_MAXIMUM (3) */
	0x15, 0x00,				/* LOGICAL_MINIMUM (0) */
	0x25, 0x01,				/* LOGICAL_MAXIMUM (1) */
	0x95, 0x03,				/* REPORT_COUNT (3) */
	0x75, 0x01,				/* REPORT_SIZE (1) */
	0x91, 0x02,				/* Output (Data,Var,Abs) */
	0x95, 0x01,				/* REPORT_COUNT (1) */
	0x75, 0x05,				/* REPORT_SIZE (5) */
	0x91, 0x01,				/* Output (Cnst,Var,Abs) */

	0x06, 0x00, 0xff,			/* Usage Page (Vendor Defined Page 1) */
	0x19, 0x06,				/* USAGE_MINIMUM (6) */
	0x29, 0x08,				/* USAGE_MAXIMUM (8) */
	0x15, 0x00,				/* LOGICAL_MINIMUM (0) */
	0x25, 0x01,				/* LOGICAL_MAXIMUM (1) */
	0x95, 0x03,				/* REPORT_COUNT (3) */
	0x75, 0x01,				/* REPORT_SIZE (1) */
	0xb1, 0x02,				/* Feature (Data,Var,Abs) */
	0x95, 0x01,				/* REPORT_COUNT (1) */
	0x75, 0x05,				/* REPORT_SIZE (5) */
	0x91, 0x01,				/* Output (Cnst,Var,Abs) */

	0xc0,				/* END_COLLECTION */
	0xc0,			/* END_COLLECTION */
};

static __u8 feature_data[] = { 1, 2 };

#define ASSERT_OK(data) ASSERT_FALSE(data)
#define ASSERT_OK_PTR(ptr) ASSERT_NE(NULL, ptr)

#define UHID_LOG(fmt, ...) do { \
	if (SHOW_UHID_DEBUG) \
		TH_LOG(fmt, ##__VA_ARGS__); \
} while (0)

static pthread_mutex_t uhid_started_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t uhid_started = PTHREAD_COND_INITIALIZER;

static pthread_mutex_t uhid_output_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t uhid_output_cond = PTHREAD_COND_INITIALIZER;
static unsigned char output_report[10];

/* no need to protect uhid_stopped, only one thread accesses it */
static bool uhid_stopped;

static int uhid_write(struct __test_metadata *_metadata, int fd, const struct uhid_event *ev)
{
	ssize_t ret;

	ret = write(fd, ev, sizeof(*ev));
	if (ret < 0) {
		TH_LOG("Cannot write to uhid: %m");
		return -errno;
	} else if (ret != sizeof(*ev)) {
		TH_LOG("Wrong size written to uhid: %zd != %zu",
			ret, sizeof(ev));
		return -EFAULT;
	} else {
		return 0;
	}
}

static int uhid_create(struct __test_metadata *_metadata, int fd, int rand_nb)
{
	struct uhid_event ev;
	char buf[25];

	sprintf(buf, "test-uhid-device-%d", rand_nb);

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_CREATE;
	strcpy((char *)ev.u.create.name, buf);
	ev.u.create.rd_data = rdesc;
	ev.u.create.rd_size = sizeof(rdesc);
	ev.u.create.bus = BUS_USB;
	ev.u.create.vendor = 0x0001;
	ev.u.create.product = 0x0a37;
	ev.u.create.version = 0;
	ev.u.create.country = 0;

	sprintf(buf, "%d", rand_nb);
	strcpy((char *)ev.u.create.phys, buf);

	return uhid_write(_metadata, fd, &ev);
}

static void uhid_destroy(struct __test_metadata *_metadata, int fd)
{
	struct uhid_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_DESTROY;

	uhid_write(_metadata, fd, &ev);
}

static int uhid_event(struct __test_metadata *_metadata, int fd)
{
	struct uhid_event ev, answer;
	ssize_t ret;

	memset(&ev, 0, sizeof(ev));
	ret = read(fd, &ev, sizeof(ev));
	if (ret == 0) {
		UHID_LOG("Read HUP on uhid-cdev");
		return -EFAULT;
	} else if (ret < 0) {
		UHID_LOG("Cannot read uhid-cdev: %m");
		return -errno;
	} else if (ret != sizeof(ev)) {
		UHID_LOG("Invalid size read from uhid-dev: %zd != %zu",
			ret, sizeof(ev));
		return -EFAULT;
	}

	switch (ev.type) {
	case UHID_START:
		pthread_mutex_lock(&uhid_started_mtx);
		pthread_cond_signal(&uhid_started);
		pthread_mutex_unlock(&uhid_started_mtx);

		UHID_LOG("UHID_START from uhid-dev");
		break;
	case UHID_STOP:
		uhid_stopped = true;

		UHID_LOG("UHID_STOP from uhid-dev");
		break;
	case UHID_OPEN:
		UHID_LOG("UHID_OPEN from uhid-dev");
		break;
	case UHID_CLOSE:
		UHID_LOG("UHID_CLOSE from uhid-dev");
		break;
	case UHID_OUTPUT:
		UHID_LOG("UHID_OUTPUT from uhid-dev");

		pthread_mutex_lock(&uhid_output_mtx);
		memcpy(output_report,
		       ev.u.output.data,
		       min(ev.u.output.size, sizeof(output_report)));
		pthread_cond_signal(&uhid_output_cond);
		pthread_mutex_unlock(&uhid_output_mtx);
		break;
	case UHID_GET_REPORT:
		UHID_LOG("UHID_GET_REPORT from uhid-dev");

		answer.type = UHID_GET_REPORT_REPLY;
		answer.u.get_report_reply.id = ev.u.get_report.id;
		answer.u.get_report_reply.err = ev.u.get_report.rnum == 1 ? 0 : -EIO;
		answer.u.get_report_reply.size = sizeof(feature_data);
		memcpy(answer.u.get_report_reply.data, feature_data, sizeof(feature_data));

		uhid_write(_metadata, fd, &answer);

		break;
	case UHID_SET_REPORT:
		UHID_LOG("UHID_SET_REPORT from uhid-dev");
		break;
	default:
		TH_LOG("Invalid event from uhid-dev: %u", ev.type);
	}

	return 0;
}

struct uhid_thread_args {
	int fd;
	struct __test_metadata *_metadata;
};
static void *uhid_read_events_thread(void *arg)
{
	struct uhid_thread_args *args = (struct uhid_thread_args *)arg;
	struct __test_metadata *_metadata = args->_metadata;
	struct pollfd pfds[1];
	int fd = args->fd;
	int ret = 0;

	pfds[0].fd = fd;
	pfds[0].events = POLLIN;

	uhid_stopped = false;

	while (!uhid_stopped) {
		ret = poll(pfds, 1, 100);
		if (ret < 0) {
			TH_LOG("Cannot poll for fds: %m");
			break;
		}
		if (pfds[0].revents & POLLIN) {
			ret = uhid_event(_metadata, fd);
			if (ret)
				break;
		}
	}

	return (void *)(long)ret;
}

static int uhid_start_listener(struct __test_metadata *_metadata, pthread_t *tid, int uhid_fd)
{
	struct uhid_thread_args args = {
		.fd = uhid_fd,
		._metadata = _metadata,
	};
	int err;

	pthread_mutex_lock(&uhid_started_mtx);
	err = pthread_create(tid, NULL, uhid_read_events_thread, (void *)&args);
	ASSERT_EQ(0, err) {
		TH_LOG("Could not start the uhid thread: %d", err);
		pthread_mutex_unlock(&uhid_started_mtx);
		close(uhid_fd);
		return -EIO;
	}
	pthread_cond_wait(&uhid_started, &uhid_started_mtx);
	pthread_mutex_unlock(&uhid_started_mtx);

	return 0;
}

static int uhid_send_event(struct __test_metadata *_metadata, int fd, __u8 *buf, size_t size)
{
	struct uhid_event ev;

	if (size > sizeof(ev.u.input.data))
		return -E2BIG;

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_INPUT2;
	ev.u.input2.size = size;

	memcpy(ev.u.input2.data, buf, size);

	return uhid_write(_metadata, fd, &ev);
}

static int setup_uhid(struct __test_metadata *_metadata, int rand_nb)
{
	int fd;
	const char *path = "/dev/uhid";
	int ret;

	fd = open(path, O_RDWR | O_CLOEXEC);
	ASSERT_GE(fd, 0) TH_LOG("open uhid-cdev failed; %d", fd);

	ret = uhid_create(_metadata, fd, rand_nb);
	ASSERT_EQ(0, ret) {
		TH_LOG("create uhid device failed: %d", ret);
		close(fd);
	}

	return fd;
}

static bool match_sysfs_device(int dev_id, const char *workdir, struct dirent *dir)
{
	const char *target = "0003:0001:0A37.*";
	char phys[512];
	char uevent[1024];
	char temp[512];
	int fd, nread;
	bool found = false;

	if (fnmatch(target, dir->d_name, 0))
		return false;

	/* we found the correct VID/PID, now check for phys */
	sprintf(uevent, "%s/%s/uevent", workdir, dir->d_name);

	fd = open(uevent, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return false;

	sprintf(phys, "PHYS=%d", dev_id);

	nread = read(fd, temp, ARRAY_SIZE(temp));
	if (nread > 0 && (strstr(temp, phys)) != NULL)
		found = true;

	close(fd);

	return found;
}

static int get_hid_id(int dev_id)
{
	const char *workdir = "/sys/devices/virtual/misc/uhid";
	const char *str_id;
	DIR *d;
	struct dirent *dir;
	int found = -1, attempts = 3;

	/* it would be nice to be able to use nftw, but the no_alu32 target doesn't support it */

	while (found < 0 && attempts > 0) {
		attempts--;
		d = opendir(workdir);
		if (d) {
			while ((dir = readdir(d)) != NULL) {
				if (!match_sysfs_device(dev_id, workdir, dir))
					continue;

				str_id = dir->d_name + sizeof("0003:0001:0A37.");
				found = (int)strtol(str_id, NULL, 16);

				break;
			}
			closedir(d);
		}
		if (found < 0)
			usleep(100000);
	}

	return found;
}

static int get_hidraw(int dev_id)
{
	const char *workdir = "/sys/devices/virtual/misc/uhid";
	char sysfs[1024];
	DIR *d, *subd;
	struct dirent *dir, *subdir;
	int i, found = -1;

	/* retry 5 times in case the system is loaded */
	for (i = 5; i > 0; i--) {
		usleep(10);
		d = opendir(workdir);

		if (!d)
			continue;

		while ((dir = readdir(d)) != NULL) {
			if (!match_sysfs_device(dev_id, workdir, dir))
				continue;

			sprintf(sysfs, "%s/%s/hidraw", workdir, dir->d_name);

			subd = opendir(sysfs);
			if (!subd)
				continue;

			while ((subdir = readdir(subd)) != NULL) {
				if (fnmatch("hidraw*", subdir->d_name, 0))
					continue;

				found = atoi(subdir->d_name + strlen("hidraw"));
			}

			closedir(subd);

			if (found > 0)
				break;
		}
		closedir(d);
	}

	return found;
}

static int open_hidraw(int dev_id)
{
	int hidraw_number;
	char hidraw_path[64] = { 0 };

	hidraw_number = get_hidraw(dev_id);
	if (hidraw_number < 0)
		return hidraw_number;

	/* open hidraw node to check the other side of the pipe */
	sprintf(hidraw_path, "/dev/hidraw%d", hidraw_number);
	return open(hidraw_path, O_RDWR | O_NONBLOCK);
}

FIXTURE(hidraw) {
	int dev_id;
	int uhid_fd;
	int hidraw_fd;
	int hid_id;
	pthread_t tid;
};
static void close_hidraw(FIXTURE_DATA(hidraw) * self)
{
	if (self->hidraw_fd)
		close(self->hidraw_fd);
	self->hidraw_fd = 0;
}

FIXTURE_TEARDOWN(hidraw) {
	void *uhid_err;

	uhid_destroy(_metadata, self->uhid_fd);

	close_hidraw(self);
	pthread_join(self->tid, &uhid_err);
}
#define TEARDOWN_LOG(fmt, ...) do { \
	TH_LOG(fmt, ##__VA_ARGS__); \
	hidraw_teardown(_metadata, self, variant); \
} while (0)

FIXTURE_SETUP(hidraw)
{
	time_t t;
	int err;

	/* initialize random number generator */
	srand((unsigned int)time(&t));

	self->dev_id = rand() % 1024;

	self->uhid_fd = setup_uhid(_metadata, self->dev_id);

	/* locate the uev, self, variant);ent file of the created device */
	self->hid_id = get_hid_id(self->dev_id);
	ASSERT_GT(self->hid_id, 0)
		TEARDOWN_LOG("Could not locate uhid device id: %d", self->hid_id);

	err = uhid_start_listener(_metadata, &self->tid, self->uhid_fd);
	ASSERT_EQ(0, err) TEARDOWN_LOG("could not start udev listener: %d", err);

	self->hidraw_fd = open_hidraw(self->dev_id);
	ASSERT_GE(self->hidraw_fd, 0) TH_LOG("open_hidraw");
}

/*
 * A simple test to see if the fixture is working fine.
 * If this fails, none of the other tests will pass.
 */
TEST_F(hidraw, test_create_uhid)
{
}

/*
 * Inject one event in the uhid device,
 * check that we get the same data through hidraw
 */
TEST_F(hidraw, raw_event)
{
	__u8 buf[10] = {0};
	int err;

	/* inject one event */
	buf[0] = 1;
	buf[1] = 42;
	uhid_send_event(_metadata, self->uhid_fd, buf, 6);

	/* read the data from hidraw */
	memset(buf, 0, sizeof(buf));
	err = read(self->hidraw_fd, buf, sizeof(buf));
	ASSERT_EQ(err, 6) TH_LOG("read_hidraw");
	ASSERT_EQ(buf[0], 1);
	ASSERT_EQ(buf[1], 42);
}

/*
 * After initial opening/checks of hidraw, revoke the hidraw
 * node and check that we can not read any more data.
 */
TEST_F(hidraw, raw_event_revoked)
{
	__u8 buf[10] = {0};
	int err;

	/* inject one event */
	buf[0] = 1;
	buf[1] = 42;
	uhid_send_event(_metadata, self->uhid_fd, buf, 6);

	/* read the data from hidraw */
	memset(buf, 0, sizeof(buf));
	err = read(self->hidraw_fd, buf, sizeof(buf));
	ASSERT_EQ(err, 6) TH_LOG("read_hidraw");
	ASSERT_EQ(buf[0], 1);
	ASSERT_EQ(buf[1], 42);

	/* call the revoke ioctl */
	err = ioctl(self->hidraw_fd, HIDIOCREVOKE, NULL);
	ASSERT_OK(err) TH_LOG("couldn't revoke the hidraw fd");

	/* inject one other event */
	buf[0] = 1;
	buf[1] = 43;
	uhid_send_event(_metadata, self->uhid_fd, buf, 6);

	/* read the data from hidraw */
	memset(buf, 0, sizeof(buf));
	err = read(self->hidraw_fd, buf, sizeof(buf));
	ASSERT_EQ(err, -1) TH_LOG("read_hidraw");
}

/*
 * Revoke the hidraw node and check that we can not do any ioctl.
 */
TEST_F(hidraw, ioctl_revoked)
{
	int err, desc_size = 0;

	/* call the revoke ioctl */
	err = ioctl(self->hidraw_fd, HIDIOCREVOKE, NULL);
	ASSERT_OK(err) TH_LOG("couldn't revoke the hidraw fd");

	/* do an ioctl */
	err = ioctl(self->hidraw_fd, HIDIOCGRDESCSIZE, &desc_size);
	ASSERT_EQ(err, -1) TH_LOG("read_hidraw");
}

/*
 * Setup polling of the fd, and check that revoke works properly.
 */
TEST_F(hidraw, poll_revoked)
{
	struct pollfd pfds[1];
	__u8 buf[10] = {0};
	int err, ready;

	/* setup polling */
	pfds[0].fd = self->hidraw_fd;
	pfds[0].events = POLLIN;

	/* inject one event */
	buf[0] = 1;
	buf[1] = 42;
	uhid_send_event(_metadata, self->uhid_fd, buf, 6);

	while (true) {
		ready = poll(pfds, 1, 5000);
		ASSERT_EQ(ready, 1) TH_LOG("poll return value");

		if (pfds[0].revents & POLLIN) {
			memset(buf, 0, sizeof(buf));
			err = read(self->hidraw_fd, buf, sizeof(buf));
			ASSERT_EQ(err, 6) TH_LOG("read_hidraw");
			ASSERT_EQ(buf[0], 1);
			ASSERT_EQ(buf[1], 42);

			/* call the revoke ioctl */
			err = ioctl(self->hidraw_fd, HIDIOCREVOKE, NULL);
			ASSERT_OK(err) TH_LOG("couldn't revoke the hidraw fd");
		} else {
			break;
		}
	}

	ASSERT_TRUE(pfds[0].revents & POLLHUP);
}

/*
 * After initial opening/checks of hidraw, revoke the hidraw
 * node and check that we can not read any more data.
 */
TEST_F(hidraw, write_event_revoked)
{
	struct timespec time_to_wait;
	__u8 buf[10] = {0};
	int err;

	/* inject one event from hidraw */
	buf[0] = 1; /* report ID */
	buf[1] = 2;
	buf[2] = 42;

	pthread_mutex_lock(&uhid_output_mtx);

	memset(output_report, 0, sizeof(output_report));
	clock_gettime(CLOCK_REALTIME, &time_to_wait);
	time_to_wait.tv_sec += 2;

	err = write(self->hidraw_fd, buf, 3);
	ASSERT_EQ(err, 3) TH_LOG("unexpected error while writing to hidraw node: %d", err);

	err = pthread_cond_timedwait(&uhid_output_cond, &uhid_output_mtx, &time_to_wait);
	ASSERT_OK(err) TH_LOG("error while calling waiting for the condition");

	ASSERT_EQ(output_report[0], 1);
	ASSERT_EQ(output_report[1], 2);
	ASSERT_EQ(output_report[2], 42);

	/* call the revoke ioctl */
	err = ioctl(self->hidraw_fd, HIDIOCREVOKE, NULL);
	ASSERT_OK(err) TH_LOG("couldn't revoke the hidraw fd");

	/* inject one other event */
	buf[0] = 1;
	buf[1] = 43;
	err = write(self->hidraw_fd, buf, 3);
	ASSERT_LT(err, 0) TH_LOG("unexpected success while writing to hidraw node: %d", err);
	ASSERT_EQ(errno, 19) TH_LOG("unexpected error code while writing to hidraw node: %d",
				    errno);

	pthread_mutex_unlock(&uhid_output_mtx);
}

int main(int argc, char **argv)
{
	return test_harness_run(argc, argv);
}
