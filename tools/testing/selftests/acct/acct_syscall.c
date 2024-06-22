// SPDX-License-Identifier: GPL-2.0

// kselftest for acct() syscall

// This tests the acct() syscall, which logs closed processes
// until deactivated or when criteria in /proc/sys/kernel/acct is met

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>

int main(void)
{
// Create file to log closed processes
char filename[] = "process_log";
FILE *fp;
fp = fopen(filename, "w");

int i = acct(filename);

if (i) {
    printf("Test Result: %s\n", strerror(errno));
    remove(filename);
    fclose(fp);
    return 1;
}

    // Create child process and wait to close
pid_t child_pid;

child_pid = fork();

if (child_pid < 0) {
    printf("Process failed\n");
    return 1;
} else if (child_pid == 0) {
    printf("Child process successfully created!\n");
} else {
    wait(NULL);
    fseek(fp, 0L, SEEK_END);
    int sz = ftell(fp);
    printf("Parent process successfully created!\n");

    i = acct(NULL);

    if (sz <= 0) {
        printf("Child process not logged");
        return 1;
    }

    printf("Test Result: Successfully logged closed process.\n");
    remove(filename);
    fclose(fp);
    return 0;
}

return 1;
}
