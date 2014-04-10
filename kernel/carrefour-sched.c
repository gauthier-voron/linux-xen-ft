#include <linux/carrefour-sched.h>

static volatile int next_proc = 0;

int modify_cpu_dest(struct task_struct* p, int chosen_cpu) {
	char  comm[TASK_COMM_LEN];
	get_task_comm(comm, p);

	if(!p->pinthread_done && strnstr(comm, APP_NAME_FILTER, TASK_COMM_LEN)) {
		p->pinthread_done = 1;
		p->pinthread_data = __sync_fetch_and_add(&next_proc, 1) % num_present_cpus();

		printk("Decision: pid %d on cpu %d\n", p->pid, p->pinthread_data);
		return p->pinthread_data;
	}
	else if (p->pinthread_done) {
		return p->pinthread_data;
	}
	else {
		return chosen_cpu;
	}
}
