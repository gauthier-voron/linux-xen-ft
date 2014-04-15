/*
Copyright (C) 2013
Fabien Gaud <fgaud@sfu.ca>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2, as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <linux/module.h> /* Needed by all modules */
#include <linux/kernel.h> /* Needed for KERN_INFO */
#include <linux/moduleparam.h>
#include <linux/sched.h>

#include <linux/slab.h>
#include <linux/random.h>

#include <linux/freezer.h>
#include <linux/kthread.h>

#define MAX_CONSIDERED_APPS   32

#define ENABLE_PERIODIC_ROTATION	0
#define ROTATION_PERIOD_MS			200

static char* considered_apps[MAX_CONSIDERED_APPS];
static int num_considered_apps = 0;
module_param_array(considered_apps, charp, &num_considered_apps, S_IRUGO);

static bool spread = 0;
module_param(spread, bool, S_IRUGO);

// 0 = pin on cpus, 1 = pin on nodes
static bool mode = 0;
module_param(mode, bool, S_IRUGO);

struct entry_t {
	int nr_tasks;
	struct list_head list_of_tasks;
};

static int * array;
static struct entry_t* nr_process;
static int array_sz;

static int total_pinned = 0;

static DEFINE_SPINLOCK(pinthread_lock);

static unsigned session_id;

static struct task_struct * work_thread = NULL;

static inline int __get_next_entry(int current_entry) {
   int min = -1;
   int i;

   if(current_entry >= 0 && nr_process[current_entry].nr_tasks == 1) {
      return current_entry; // No need to move it
   }

   // Find the min
   for(i = 0; i < array_sz; i++) {
      int entry = array[i];

      //printk("%d ", nr_process[i]);
      if((min == -1 || nr_process[entry].nr_tasks < nr_process[min].nr_tasks)) {
         if((!mode && cpu_online(entry)) || (mode && node_online(entry))) {
            min = entry;
         }
      }
   }
   //printk("\n");

   if(min == -1) {
      printk(KERN_CRIT "(%s:%d) Wow, that's a bug!\n",  __FUNCTION__, __LINE__);
      min = 0;
   }

   return min;
}

void new_process_cb(struct task_struct* p, pinthread_op_t op) {
   int i, found = 0;
   char comm[TASK_COMM_LEN];

   const struct cpumask* dstp_p = NULL;

   if(!p) {
      return;
   }

	get_task_comm(comm, p);

	if(num_considered_apps > 0) {
		for(i = 0; i < num_considered_apps; i++) {
			char * app = considered_apps[i];
			if(strnstr(comm, app, TASK_COMM_LEN)) {
				found = 1;
				break;
			}
		}
	}
	else {
		found = 1;
	}

	if(op == CLONE || p->pinthread_session_id != session_id) {
		p->pinthread_done = 0;
	}

	if(!found && (op != TASKCOMM || !p->pinthread_done)) {
		return;
	}

   spin_lock(&pinthread_lock);

	//printk("%s has been called for pid: %d (name = %s)\n", (op == CLONE) ? "Clone" : (op == TASKCOMM ? "Taskcomm" : "Exit"), p->pid, comm);

   if(op == EXIT) {
		nr_process[p->pinthread_data].nr_tasks--;
		list_del(&p->pinthread_list);

		p->pinthread_done = 0;

		printk("[TOTAL %5d] EXIT has been called for pid: %d (name = %s, core/node = %d)\n", --total_pinned, p->pid, comm, p->pinthread_data);
		if(unlikely(nr_process[p->pinthread_data].nr_tasks < 0)) {
			printk(KERN_CRIT "(%s:%d) Wow, that's a bug!\n",  __FUNCTION__, __LINE__);
		}
   }
   else if(op == CLONE || (op == TASKCOMM && found)) {
		int entry = __get_next_entry(p->pinthread_done ? p->pinthread_data : -1);

		if(!p->pinthread_done || (p->pinthread_done && entry != p->pinthread_data)) {
			if(mode) {
				// Node mode
				dstp_p = cpumask_of_node(entry);
			}
			else {
				// CPU mode
				dstp_p = get_cpu_mask(entry);
			}

			if(!p->pinthread_done) {
				total_pinned++;

				printk("[TOTAL %5d] Assigning pid %d (%s) to core/node %d\n", total_pinned, p->pid,  comm, entry);
			}
			else {
				printk("[TOTAL %5d] Moving pid %d (%s) to core/node %d\n", total_pinned, p->pid,  comm, entry);
				nr_process[p->pinthread_data].nr_tasks--;
				list_del(&p->pinthread_list);
			}

			p->pinthread_done = 1;
			p->pinthread_session_id = session_id;
			p->pinthread_data = entry;

			nr_process[entry].nr_tasks++;
			list_add_tail(&p->pinthread_list, &nr_process[entry].list_of_tasks);
		}
	}
	else if (op == TASKCOMM) {
		if(!p->pinthread_done)
			BUG();

		printk("[TOTAL %5d] Pid %d has changed its name to %s. Not matched anymore. Stop pinning.\n", total_pinned, p->pid, comm);

		dstp_p = cpu_possible_mask;
		p->pinthread_done = 0;

		nr_process[p->pinthread_data].nr_tasks--;
		list_del(&p->pinthread_list);
		total_pinned--;
	}

   if(dstp_p) {
      sched_setaffinity(p->pid, dstp_p);
   }

   spin_unlock(&pinthread_lock);
}

#if ENABLE_PERIODIC_ROTATION
static void __dump_tasks(void) {
	int i;
	for(i = 0; i < array_sz; i++) {
		struct task_struct *p, *n;

		printk("On core %d:", i);
		list_for_each_entry_safe(p, n, &nr_process[i].list_of_tasks, pinthread_list) {
			printk(" %d", p->pid);
		}
		printk("\n");
	}
}

static int rotate_tasks(void) {
   int i;
	int size = nr_process[0].nr_tasks;

	int err = 0;

	if(total_pinned < num_online_cpus()) {
		return 0;
	}

	//__dump_tasks();

   for(i = 0; i < array_sz; i++) {
		int next = (i + 1) % array_sz;
		int next_size_save = nr_process[next].nr_tasks;
		int j = 0;

		struct task_struct *p, *n;

		if(size > nr_process[i].nr_tasks) {
			__dump_tasks();
			printk(KERN_CRIT "BUG: Size = %d, nr tasks = %d\n", size, nr_process[i].nr_tasks);
			err = -1;
			break;
		}

		//printk("Moving %d tasks from core %d (%d) to core %d (%d)\n", size, i, nr_process[i].nr_tasks, next, nr_process[next].nr_tasks);

		list_for_each_entry_safe(p, n, &nr_process[i].list_of_tasks, pinthread_list) {
			const struct cpumask* dstp_p = NULL;

			if(j == size) {
				break;
			}

			nr_process[i].nr_tasks--;
			nr_process[next].nr_tasks++;

			list_move_tail(&p->pinthread_list, &nr_process[next].list_of_tasks);

			p->pinthread_data = next;

			// Do the actuall pinning
			if(mode) {
				// Node mode
				dstp_p = cpumask_of_node(array[next]);
			}
			else {
				// CPU mode
				dstp_p = get_cpu_mask(array[next]);
			}
			sched_setaffinity(p->pid, dstp_p);

			j++;
		}

		if(j != size) {
			printk(KERN_CRIT "Weird. We moved %d but estimated size was %d (i = %d)\n", j, size, i);
			__dump_tasks();
			err = -1;
			break;
		}

		size = next_size_save;
   }

	return err;
}

static int pinthread_rotate_thread(void *nothing) {
	static DECLARE_WAIT_QUEUE_HEAD(pinthread_wq);
	int err = 0;

   while(!kthread_should_stop() && !err) {
		wait_event_freezable_timeout(pinthread_wq, kthread_should_stop(), msecs_to_jiffies(ROTATION_PERIOD_MS));

		if(kthread_should_stop()) {
			break;
		}

		spin_lock(&pinthread_lock);
		err = rotate_tasks();

		if(err) {
			printk(KERN_CRIT "Error detected. Exiting...\n");
			work_thread = NULL;
		}

		spin_unlock(&pinthread_lock);
	}

   return 0;
}
#endif

static int __init pinthreads_init_module(void) {
   int i;
   int cpu, node;

   printk("Pinthreads: considered_apps:");
   if(num_considered_apps) {
      for(i = 0; i < num_considered_apps; i++) {
         printk("\n\t%s", considered_apps[i]);
      }
      printk("\n");
   }
   else {
      printk(" all\n");
   }

   session_id = random32();
   printk("Session id: %u\n", session_id);

   printk("Mode: %d (%s)\n", mode, mode ? "Node": "Core");
   if(mode) {
      array_sz = num_possible_nodes();
   }
   else {
      array_sz = num_present_cpus();
   }

   array = kmalloc(array_sz * sizeof(int), GFP_KERNEL);
   nr_process = kmalloc(array_sz * sizeof(struct entry_t), GFP_KERNEL | __GFP_ZERO);

   if(!array || !nr_process) {
      printk(KERN_CRIT "Cannot allocate memory!\n");
      return -1;
   }

   if(mode) {
      for_each_node(node) {
         array[node] = node;
      }
   }
   else {
      if(!spread) {
         i = 0;
         for_each_node(node) {
            for_each_cpu(cpu, cpumask_of_node(node)) {
               if(i == array_sz) {
                  printk(KERN_CRIT "Trying to add more cpu than present!\n");
                  return -1;
               }

               array[i] = cpu;
               i++;
            }
         }
      }
      else {
         int next_node = 0;

         for(i = 0; i < array_sz; i++) {
            int idx = (i / num_possible_nodes()) + 1;
            int found = 0;

            for_each_present_cpu(cpu) {
               if(cpu_to_node(cpu) == next_node) {
                  found++;

                  if(found == idx){
                     array[i] = cpu;
                     break;
                  }
               }
            }

            if(i == array_sz) {
               printk(KERN_CRIT "Strange bug!\n");
               return -1;
            }

            next_node = (next_node + 1) % num_possible_nodes();
         }
      }
   }

	for(i = 0; i < array_sz; i++) {
		INIT_LIST_HEAD(&nr_process[i].list_of_tasks);
	}

   pinthread_callback = &new_process_cb;

#if ENABLE_PERIODIC_ROTATION
	work_thread = kthread_run(pinthread_rotate_thread, NULL, "pinthreads_rotated");
   if(IS_ERR(work_thread)) {
      printk(KERN_CRIT "pinthreads_rotated creation failed\n");
		return -1;
	}
#endif

   return 0;
}

static void __exit pinthreads_exit_module(void) {
   spin_lock(&pinthread_lock);
   pinthread_callback = NULL;

	if(work_thread) {
		kthread_stop(work_thread);
	}

   spin_unlock(&pinthread_lock);
}

module_init(pinthreads_init_module);
module_exit(pinthreads_exit_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Fabien Gaud");
MODULE_DESCRIPTION("Kernel module to pin threads on cores");
