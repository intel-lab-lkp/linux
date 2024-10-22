// SPDX-License-Identifier: GPL-2.0

#include <stdio.h>
#include <string.h>
#include <fcntl.h>


struct load_avg {
	float avg10;
	float avg60;
	float avg300;
	unsigned long long total;
};

struct pressure {
	struct load_avg some;
	struct load_avg full;
};


int psi_get_data_from_proc_pressure(const char *path, struct pressure *p)
{
	FILE *fp;
	int rc = -1;
	int ret = 0;

	if (path == NULL || p == NULL)
		return -1;

	fp = fopen(path, "r");
	if (fp == NULL)
		return -1;

	while (!feof(fp)) {
		rc = fscanf(fp, "some avg10=%f avg60=%f avg300=%f total=%llu\n",
			&p->some.avg10, &p->some.avg60, &p->some.avg300, &p->some.total);
		if (rc < 1) {
			ret = -1;
			break;
		}

		/* Note: In some cases (cpu) full may not exists */
		rc = fscanf(fp, "full avg10=%f avg60=%f avg300=%f total=%llu\n",
			&p->full.avg10, &p->full.avg60, &p->full.avg300, &p->full.total);
		/* We don't care about full case. This is needed to avoid warnings */
		rc = 0;
	}

	fclose(fp);

	return ret;
}

int main(int argc, char *argv[])
{
	int ret;
	struct pressure rs = {0,};
	char path[32];

	if (argc < 2) {
		fprintf(stderr, "usage: %s <path>\n", argv[0]);
		return -1;
	}

	memset(&rs, 0, sizeof(rs));
	printf("Pressure data: %s\n", argv[1]);
	snprintf(path, sizeof(path)-1, "/proc/pressure/%s", argv[1]);

	ret = psi_get_data_from_proc_pressure(path, &rs);
	if (ret < 0) {
		printf("PSI <%s>: FAIL\n", argv[1]);
		return -1;
	}
	printf("Some Avg10   = %5.2f\n", rs.some.avg10);
	printf("Some Avg60   = %5.2f\n", rs.some.avg60);
	printf("Some Avg300  = %5.2f\n", rs.some.avg300);
	printf("Some Total  = %llu\n", rs.some.total);
	printf("Full Avg10  = %5.2f\n", rs.full.avg10);
	printf("Full Avg60  = %5.2f\n", rs.full.avg60);
	printf("Full Avg300 = %5.2f\n", rs.full.avg300);
	printf("Full Total  = %llu\n", rs.full.total);


	return 0;
}
