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

static int * cpu_array;
static int cpu_array_nr;
static int total_pinned = 0;

static int * cpu_nr_process;
static DEFINE_SPINLOCK(pinthread_lock);

static unsigned session_id;

static inline int __get_next_cpu(int current_cpu) {
   int min_cpu = -1;
   int i;

   if(current_cpu >= 0 && cpu_nr_process[current_cpu] == 1) {
      return current_cpu; // No need to move it
   }

   // Find the min
   for(i = 0; i < cpu_array_nr; i++) {
      int cpu = cpu_array[i];

      //printk("%d ", cpu_nr_process[i]); 
      if((cpu_nr_process[cpu] < cpu_nr_process[min_cpu] || min_cpu == -1) && cpu_online(cpu)) {
         min_cpu = cpu;
      }
   }
   //printk("\n");

   if(min_cpu == -1) {
      printk(KERN_WARNING "(%s:%d) Wow, that's a bug!\n",  __FUNCTION__, __LINE__);
      min_cpu = 0;
   }

   return min_cpu;
}

void new_process_cb(struct task_struct* p, pinthread_op_t op) {
   int i;
   char comm[TASK_COMM_LEN];

   struct cpumask dstp;
   const struct cpumask* dstp_p = NULL;

   if(!p) {
      return;
   }

   get_task_comm(comm, p);

   spin_lock(&pinthread_lock);

   if(op == EXIT) {
      if(p->pinthread_done && p->pinthread_session_id == session_id) {
         cpu_nr_process[p->pinthread_core]--;
         p->pinthread_done = 0;

         printk("[TOTAL %5d] EXIT has been called for pid: %d (name = %s, core = %d)\n", --total_pinned, p->pid, comm, p->pinthread_core);
         if(unlikely(cpu_nr_process[p->pinthread_core] < 0)) {
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
         int cpu = __get_next_cpu(p->pinthread_done ? p->pinthread_core : -1);

         if(!p->pinthread_done || (p->pinthread_done && cpu != p->pinthread_core)) {
            cpumask_clear(&dstp);
            cpumask_set_cpu(cpu, &dstp);
            dstp_p = &dstp;

            if(!p->pinthread_done) {
               total_pinned++;

               printk("[TOTAL %5d] Assigning pid %d (%s) to core %d\n", total_pinned, p->pid,  comm, cpu);
            }
            else {
               printk("[TOTAL %5d] Moving pid %d (%s) to core %d\n", total_pinned, p->pid,  comm, cpu);
               cpu_nr_process[p->pinthread_core]--;
            }

            p->pinthread_done = 1;
            p->pinthread_session_id = session_id;
            p->pinthread_core = cpu;

            cpu_nr_process[cpu]++;
         }
      }
      else if (op == TASKCOMM && p->pinthread_done) {
         if(i == num_considered_apps && num_considered_apps > 0) {
            printk("[TOTAL %5d] Pid %d has changed its name to %s. Not matched anymore. Stop pinning.\n", total_pinned, p->pid, comm);

            dstp_p = cpu_possible_mask;
            p->pinthread_done = 0;

            cpu_nr_process[p->pinthread_core]--;
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

   cpu_array_nr = num_present_cpus();
   cpu_array = kmalloc(cpu_array_nr * sizeof(int), GFP_KERNEL);
   cpu_nr_process = kmalloc(cpu_array_nr * sizeof(int), GFP_KERNEL | __GFP_ZERO);

   if(!cpu_array || !cpu_nr_process) {
      printk(KERN_CRIT "Cannot allocate memory!\n");
      return -1;
   }

   if(!spread) {
      i = 0;
      for_each_node(node) {
         for_each_cpu(cpu, cpumask_of_node(node)) {
            if(i == cpu_array_nr) {
               printk(KERN_CRIT "Trying to add more cpu than present!\n");
               return -1;
            }

            cpu_array[i] = cpu;
            i++;
         }
      }
   }
   else {
      int next_node = 0;

      for(i = 0; i < cpu_array_nr; i++) {
         int idx = (i / num_possible_nodes()) + 1;
         int found = 0;

         for_each_present_cpu(cpu) {
            if(cpu_to_node(cpu) == next_node) {
               found++;

               if(found == idx){
                  cpu_array[i] = cpu;
                  break;
               }
            }
         }
         
         if(i == cpu_array_nr) {
            printk(KERN_CRIT "Strange bug!\n");
            return -1;
         }

         next_node = (next_node + 1) % num_possible_nodes();
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
