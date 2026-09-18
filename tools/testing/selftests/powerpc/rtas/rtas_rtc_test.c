// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTAS Real-Time Clock Test
 * Copyright (C) 2026 IBM Corporation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include "utils.h"

#define _GNU_SOURCE
#define RTC_PATH "/sys/class/rtc/rtc0/time"
#define RTC_DATE_PATH "/sys/class/rtc/rtc0/date"

static int read_rtc_time(time_t *rtc_time)
{
	FILE *fp;
	struct tm tm = {0};
	char buf[64];

	fp = fopen(RTC_PATH, "r");
	if (!fp) {
		perror("fopen RTC_PATH");
		return -1;
	}

	if (!fgets(buf, sizeof(buf), fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	/* Parse HH:MM:SS format */
	if (sscanf(buf, "%d:%d:%d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 3)
		return -1;

	/* Read date */
	fp = fopen(RTC_DATE_PATH, "r");
	if (!fp)
		return -1;

	if (!fgets(buf, sizeof(buf), fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	/* Parse YYYY-MM-DD format */
	if (sscanf(buf, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3)
		return -1;

	tm.tm_year -= 1900;  /* years since 1900 */
	tm.tm_mon -= 1;      /* months since January */

	*rtc_time = timegm(&tm);
	return 0;
}

int main(void)
{
	time_t rtc_time1, rtc_time2, sys_time;
	int diff;

	test_harness_set_timeout(10);

	/* Test 1: Verify RTC device exists */
	SKIP_IF(access(RTC_PATH, R_OK) != 0);

	/* Test 2: Read RTC time */
	FAIL_IF(read_rtc_time(&rtc_time1) != 0);

	/* Test 3: Compare with system time */
	sys_time = time(NULL);
	diff = abs((int)(rtc_time1 - sys_time));
	FAIL_IF_MSG(diff > 2, "RTC time differs from system time by %d seconds", diff);

	/* Test 4: Verify year > 2020 */
	struct tm *tm = localtime(&rtc_time1);
	FAIL_IF_MSG(tm->tm_year + 1900 < 2020, "RTC year is %d (expected >= 2020)", 
	            tm->tm_year + 1900);

	/* Test 5: Verify time advances */
	sleep(2);
	FAIL_IF(read_rtc_time(&rtc_time2) != 0);
	FAIL_IF_MSG(rtc_time2 <= rtc_time1, "RTC time did not advance");

	printf("RTAS RTC test passed\n");
	return 0;
}
