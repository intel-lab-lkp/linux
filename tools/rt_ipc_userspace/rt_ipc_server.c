#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#include "rt_ipc_action.h"

void entry(unsigned cmd, rt_ipcinfo_t *info);

int main(void)
{
    struct rt_ipc_action act;

    act.activation = entry;
    int ret = rt_ipc_action(&act);

    if (ret < 0) {
        pr_err("fatal: rt ipc action error\n");
        return -EINVAL;
    }

    pr_info("pid: %d entry: %p\n", getpid(), entry);

    while(1)
    {
        sleep(INT_MAX);
        pr_info("wait for the signal\n");
    }

    return 0;
}

volatile int g_test;
#define CONFIG_FILE_OP 0

static volatile unsigned long long cnt;

void entry(unsigned int cmd, rt_ipcinfo_t *info)
{
    g_test = gettid();
#if CONFIG_FILE_OP
    FILE *f = fopen("/tmp/123.txt", "w+");
    if (f == NULL) {
	    pr_info("fopen failed with error: %d\n", errno);
	    return;
    }
    char buf[100];
    int a = 0;
    int b = 1;
    a = b;
    b = a;
    g_test = getpid();

    snprintf(buf, 100, "receive cmd %x pid: %d g_test; %d\n", cmd, getpid(), g_test);
    fwrite(buf, strlen(buf) + 1, 1, f);
    fclose(f);
#endif
    info->write_size = cnt;
    if (++cnt % 10000 == 0)
        pr_info("message from client: pid: %d cmd: %08x cnt: %lld, info->write_size: %ld\n", g_test, cmd, cnt, info->write_size);
    //sleep(5);
}
