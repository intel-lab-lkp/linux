#include <linux/compiler.h>

#define DEFINE_RATELIMIT_STATE(name, interval_init, burst_init) \
	int name = 0;						\
	ASSERT_STATIC_STORAGE(name)

#define __ratelimit(x) (*(x))

