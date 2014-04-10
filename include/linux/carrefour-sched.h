#ifndef __CARREFOUR_SCHED__
#define __CARREFOUR_SCHED__

#include <linux/sched.h>
#include <linux/carrefour-stats.h>

#define ENABLE_SMART_INITIAL_PLACEMENT	0
#define DISABLE_WAKEUP_LB					0
#define DISABLE_IDLE_LB						0
#define DISABLE_PERIODIC_LB				0
#define DISABLE_ACTIVE_LB_CPU_STOP		0

#define VERBOSE								0

int modify_cpu_dest(struct task_struct* p, int chosen_cpu);

#if VERBOSE
#define DEBUG_TASK_PLACEMENT(msg, args...) { \
	char comm[TASK_COMM_LEN]; \
	get_task_comm(comm, p); \
	if(strnstr(comm, APP_NAME_FILTER, TASK_COMM_LEN)) { \
		printk("[%s:%d] " msg, __FUNCTION__, __LINE__, ##args); \
	} \
}

#else
#define DEBUG_TASK_PLACEMENT(msg, args...) do {} while(0)
#endif

#endif
