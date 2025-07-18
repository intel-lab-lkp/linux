#define _GNU_SOURCE
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#include "rt_ipc_action.h"

#define PAGE_SIZE (1 << 12)

#define STACK_SIZE (PAGE_SIZE << 1)

/*
int syscall(SYS_rt_sigqueueinfo, pid_t tgid, int sig, siginfo_t *info);
int syscall(SYS_rt_tgsigqueueinfo, pid_t tgid, pid_t tid, int sig, siginfo_t *info);
*/

#define handle_error_en(en, msg) \
               do { errno = en; perror(msg); exit(EXIT_FAILURE); } while (0)

static volatile unsigned long long cnt;

static void *thread_entry(void *args)
{
    struct rt_ipcinfo info;
    unsigned int cmd = 0x5a5a;
    int ret;
    info.write_size = 1234;

    while (cnt++ != 100000) {
        ret = syscall(SYS_rt_ipc_invoke, *(int *)args, cmd, &info);
        if (ret < 0)
            printf("exit from migrating %d\n", gettid());
        //sleep(1);
    }
    printf("rt_ipc client: %ld\n", info.write_size);
    //sleep(0);
    return NULL;
}

int find_pid_by_name(const char *process_name) {
    char command[256];
    snprintf(command, sizeof(command), "pgrep %s", process_name);

    FILE *fp = popen(command, "r");
    if (fp == NULL) {
        perror("Error opening pipe");
        return -1;
    }

    int pid;
    if (fscanf(fp, "%d", &pid) == 1) {
        pclose(fp);
        return pid;
    } else {
        pclose(fp);
        return -1; // Process not found
    }
}

int main(int argc, char **argv)
{
    pid_t pid;
    struct timeval t1, t2;

    pid = find_pid_by_name("server_rt_ipc");
    if (pid < 0) {
        return pid;
    }

    printf("client pid %d send syscall SYS_rt_ipc_migrating to server pid: %d\n", getpid(), pid);

    gettimeofday(&t1, NULL);

    thread_entry(&pid);

    gettimeofday(&t2, NULL);

    __time_t sec = t2.tv_sec - t1.tv_sec;
    __suseconds_t usec = sec * 1000000 + t2.tv_usec - t1.tv_usec;

    printf("time usage sec: %lds usec: %ldus pid %d\n", sec, usec, getpid());

    return 0;
}
