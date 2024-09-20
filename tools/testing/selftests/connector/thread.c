// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: Anjali Kulkarni <anjali.k.kulkarni@oracle.com>
 *
 * Copyright (c) 2024 Oracle and/or its affiliates.
 */

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>

/*
 * This code tests a thread exit notification when thread exits abnormally.
 * Normally, when a thread exits abnormally, the kernel is not aware of the
 * exit code. This is usually only conveyed from child to parent via the
 * pthread_exit() and pthread_join() calls. Sometimes, however, a parent
 * process cannot monitor all child processes via pthread_join(), particularly
 * when there is a huge amount of child processes. In this case, the parent
 * has created the child with PTHREAD_CREATE_DETACHED attribute.
 * To fix this problem, either when child wants to convey non-zero exit via
 * pthread_exit() or in a signal handler, the child can notify the kernel's
 * connector module it's exit status via a netlink call with new type
 * PROC_CN_MCAST_NOTIFY. (Implemented in the thread_filter.c file).
 * This will send the exit code from the child to the kernel, which the kernel
 * can later return to proc_filter program when the child actually exits.
 * To test this usecase:
 * Compile:
 *	make thread
 *	make proc_filter
 * To see non-zero exit notifications, run:
 *	./proc_filter -f
 * Start the threads code, creating 2 threads, in another window:
 *	./threads
 * Note the 2 child thread IDs reported above
 * Send SIGSEGV signal to the child handling SIGSEGV:
 *	kill -11 <child1-tid>
 * Watch the event being notified with exit code 11 to proc_filter
 * Watch child 2 tid being notified with exit code 1 (value defined in code)
 * to proc_filter
 */

extern int notify_netlink_thread_exit(unsigned int exit_code);

static void sigsegvh(int sig)
{
	unsigned int exit_code = (unsigned int) sig;
	/*
	 * Send any non-zero value to get a notification. Here we are
	 * sending the signal number for SIGSEGV which is 11
	 */
	notify_netlink_thread_exit(exit_code);
}

void *threadc1(void *ptr)
{
	signal(SIGSEGV, sigsegvh);
	printf("Child 1 thread id %d, handling SIGSEGV\n", gettid());
	sleep(50);
	pthread_exit(NULL);
}

void *threadc2(void *ptr)
{
	printf("Child 2 thread id %d\n", gettid());
	sleep(2);
	notify_netlink_thread_exit(1);
	pthread_exit(NULL);
}

int main(int argc, char **argv)
{
	pthread_t thread1, thread2;
	pthread_attr_t attr1, attr2;

	pthread_attr_init(&attr1);
	pthread_attr_setdetachstate(&attr1, PTHREAD_CREATE_DETACHED);
	pthread_create(&thread1, &attr1, *threadc1, NULL);

	pthread_attr_init(&attr2);
	pthread_attr_setdetachstate(&attr2, PTHREAD_CREATE_DETACHED);
	pthread_create(&thread2, &attr2, *threadc2, NULL);

	sleep(50);
	exit(0);
}
