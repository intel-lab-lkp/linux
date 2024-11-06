// SPDX-License-Identifier: GPL-2.0

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int nfiles = argc - 1;
	char *nread_str;
	int nread;
	int delay;
	int *fds;
	int i, j;

	nread_str = getenv("FASTCAT_READ_COUNT");
	if (!nread_str)
		nread = 1;
	else
		nread = atoi(nread_str);

	nread_str = getenv("FASTCAT_DELAY_USEC");
	if (!nread_str)
		delay = 0;
	else
		delay = atoi(nread_str);

	if (nfiles < 1)
		exit(1);

	fds = malloc(sizeof(*fds) * (argc));
	if (!fds) {
		perror("malloc");
		exit(1);
	}

	printf("opening %d files\n", nfiles);

	for (i = 1; i < argc; i++) {
		fds[i - 1] = open(argv[i], O_RDONLY);
		if (fds[i - 1] < 0) {
			perror(argv[i]);
			exit(1);
		}
	}

	printf("reading %d files %d times\n", nfiles, nread);

	for (j = 0; j < nread; j++) {
		for (i = 0; i < nfiles; i++) {
			// Assumed to be large enough for any output of
			// mbm_*_bytes
			char buf[40];
			ssize_t r;

			r = pread(fds[i], buf, sizeof(buf), 0);
			if (r < 0) {
				perror(argv[i + 1]);
				exit(1);
			}
		}
		if (delay)
			// Try to read cold...
			usleep(delay);
	}

	printf("closing %d files\n", nfiles);
	for (i = 0; i < nfiles; i++)
		close(fds[i]);

	return 0;
}
