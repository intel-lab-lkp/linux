#ifndef __RT_IPC_ACTION_H
#define __RT_IPC_ACTION_H
#include <stddef.h>

#define RT_IPC_ACTIVATION_THREAD_NUM 8
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

typedef struct rt_ipcinfo {
	size_t write_size;
	size_t write_buffer;
	size_t read_size;
	size_t read_buffer;
} rt_ipcinfo_t;

struct rt_ipc_action {
	void (*activation)(unsigned cmd, rt_ipcinfo_t *info);
	void (*rt_ipc_restorer)(void);
	int flags;
};

#define __NR_rt_ipc_register 468
#define __NR_rt_ipc_invoke 469
#define __NR_rt_ipc_return 470

#define SYS_rt_ipc_register __NR_rt_ipc_register
#define SYS_rt_ipc_invoke __NR_rt_ipc_invoke
#define SYS_rt_ipc_return __NR_rt_ipc_return

#define NONE "\033[m"
#define RED "\033[0;32;31m"
#define LIGHT_RED "\033[1;31m"
#define GREEN "\033[0;32;32m"
#define LIGHT_GREEN "\033[1;32m"
#define BLUE "\033[0;32;34m"
#define LIGHT_BLUE "\033[1;34m"
#define DARY_GRAY "\033[1;30m"
#define CYAN "\033[0;36m"
#define LIGHT_CYAN "\033[1;36m"
#define PURPLE "\033[0;35m"
#define LIGHT_PURPLE "\033[1;35m"
#define BROWN "\033[0;33m"
#define YELLOW "\033[1;33m"
#define LIGHT_GRAY "\033[0;37m"
#define WHITE "\033[1;37m"

#if 0
#define pr_info(fmt, ...) printf("[%s:%d]: " fmt, __func__, __LINE__, ##__VA_ARGS__)
#define pr_err(fmt, ...) printf(RED"[%s:%d]: " fmt NONE, __func__, __LINE__, ##__VA_ARGS__)
#else
#define pr_info(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...) printf(RED fmt NONE, ##__VA_ARGS__)
#endif

int rt_ipc_action(const struct rt_ipc_action *act);

#endif /* __RT_IPC_ACTION_H */
