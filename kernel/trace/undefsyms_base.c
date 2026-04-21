#include <linux/atomic.h>
#include <linux/string.h>
#include <asm/page.h>
static char page[PAGE_SIZE] __aligned(PAGE_SIZE);
void undefsyms_base(void *p, int n);
void undefsyms_base(void *p, int n) {
	char buffer[256] = { 0 };
	u32 u = 0;
	memset((char * volatile)page, 8, PAGE_SIZE);
	memset((char * volatile)buffer, 8, sizeof(buffer));
	memcpy((void * volatile)p, buffer, sizeof(buffer));
	cmpxchg((u32 * volatile)&u, 0, 8);
	WARN_ON(n == 0xdeadbeef);
}
