// SPDX-License-Identifier: GPL-2.0
int ikm_list_monitors(char *container);
int ikm_run_monitor(char *monitor, int argc, char **argv);
int __ikm_read_enable(char *monitor_name);
int ikm_enable(char *monitor_name);
int ikm_disable(char *monitor_name);
void ikm_set_reactor(char *monitor_name);
void ikm_reset_reactor(char *monitor_name);
