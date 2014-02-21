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

#define MAX_CONSIDERED_APPS   32

static char* considered_apps[MAX_CONSIDERED_APPS];
static int num_considered_apps = 0;
module_param_array(considered_apps, charp, &num_considered_apps, S_IRUGO);

static bool spread = 0;
module_param(spread, bool, S_IRUGO);

// 0 = pin on cpus, 1 = pin on nodes
static bool mode = 0;
module_param(mode, bool, S_IRUGO);

static int * array;
static int * nr_process;
static int array_sz;

static int total_pinned = 0;

static DEFINE_SPINLOCK(pinthread_lock);

static unsigned session_id;

static inline int __get_next_entry(int current_entry) {
   int min = -1;
   int i;

   if(current_entry >= 0 && nr_process[current_entry] == 1) {
      return current_entry; // No need to move it
   }

   // Find the min
   for(i = 0; i < array_sz; i++) {
      int entry = array[i];

      //printk("%d ", nr_process[i]); 
      if((nr_process[entry] < nr_process[min] || min == -1)) {
         if((!mode && cpu_online(entry)) || (mode && node_online(entry))) {
            min = entry;
         }
      }
   }
   //printk("\n");

   if(min == -1) {
      printk(KERN_WARNING "(%s:%d) Wow, that's a bug!\n",  __FUNCTION__, __LINE__);
      min = 0;
   }

   return min;
}

void new_process_cb(struct task_struct* p, pinthread_op_t op) {
   int i;
   char comm[TASK_COMM_LEN];

   const struct cpumask* dstp_p = NULL;

   if(!p) {
      return;
   }

   get_task_comm(comm, p);

   spin_lock(&pinthread_lock);

   if(op == EXIT) {
      if(p->pinthread_done && p->pinthread_session_id == session_id) {
         nr_process[p->pinthread_data]--;
         p->pinthread_done = 0;

         printk("[TOTAL %5d] EXIT has been called for pid: %d (name = %s, core/node = %d)\n", --total_pinned, p->pid, comm, p->pinthread_data);
         if(unlikely(nr_process[p->pinthread_data] < 0)) {
            printk(KERN_WARNING "(%s:%d) Wow, that's a bug!\n",  __FUNCTION__, __LINE__);
         }
      }
      else if (p->pinthread_done && p->pinthread_session_id != session_id) {
         printk("pinthreads session id %u does not match current session id %u\n", p->pinthread_session_id, session_id);
      }
   }
   else {
      // CLONE || TASKCOMM
      //printk("%s has been called for pid: %d (name = %s)\n", clone ? "Clone" : "Taskcomm", p->pid, comm);

      for(i = 0; i < num_considered_apps; i++) {
         char * app = considered_apps[i];
         if(strnstr(comm, app, TASK_COMM_LEN)) {
            break;
         }
      }

      if(op == CLONE || p->pinthread_session_id != session_id) {
         p->pinthread_done = 0;
      }

      if((num_considered_apps == 0 && op == CLONE) || (i < num_considered_apps)) {
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
               nr_process[p->pinthread_data]--;
            }

            p->pinthread_done = 1;
            p->pinthread_session_id = session_id;
            p->pinthread_data = entry;

            nr_process[entry]++;
         }
      }
      else if (op == TASKCOMM && p->pinthread_done) {
         if(i == num_considered_apps && num_considered_apps > 0) {
            printk("[TOTAL %5d] Pid %d has changed its name to %s. Not matched anymore. Stop pinning.\n", total_pinned, p->pid, comm);

            dstp_p = cpu_possible_mask;
            p->pinthread_done = 0;

            nr_process[p->pinthread_data]--;
            total_pinned--;
         }
      }
   }

   spin_unlock(&pinthread_lock);

   if(dstp_p) {
      sched_setaffinity(p->pid, dstp_p);
   }
}

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
   nr_process = kmalloc(array_sz * sizeof(int), GFP_KERNEL | __GFP_ZERO);

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
   
   pinthread_callback = &new_process_cb;
 
   return 0;
}

static void __exit pinthreads_exit_module(void) {
   spin_lock(&pinthread_lock);
   pinthread_callback = NULL;
   spin_unlock(&pinthread_lock);
}

module_init(pinthreads_init_module);
module_exit(pinthreads_exit_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Fabien Gaud");
MODULE_DESCRIPTION("Kernel module to pin threads on cores");
