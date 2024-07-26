#include <linux/compiler.h>
#include <unistd.h>
#include "../tests.h"

static int offcpu(int argc __maybe_unused, const char **argv __maybe_unused)
{
	/* get pass initial delay */
	sleep(1);

	/* what we want to collect as a direct sample */
	sleep(2);

	return 0;
}

DEFINE_WORKLOAD(offcpu);
