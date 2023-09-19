#include <sys/prctl.h>
#include <stdlib.h>

int main()
{
	/* Test if KSM is enabled for the process. */
	int ksm = prctl(68, 0, 0, 0, 0);
	exit(ksm == 1 ? 0 : 1);
}
