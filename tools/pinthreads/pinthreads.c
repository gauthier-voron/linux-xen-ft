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

#define MAX_CONSIDERED_APPS   32

static char* considered_apps[MAX_CONSIDERED_APPS];
static int num_considered_apps = 0;
module_param_array(considered_apps, charp, &num_considered_apps, S_IRUGO);

static bool spread = 0;
module_param(spread, bool, S_IRUGO);

static int * cpu_array;
static int cpu_array_nr;
static int cpu_array_next = 0;

static inline int __get_next_cpu(void) {
   int cpu = cpu_array[cpu_array_next]; 
   // Find the next online cpu
   while(!cpu_online(cpu)) {
      cpu_array_next = (cpu_array_next + 1) % cpu_array_nr;
      cpu = cpu_array[cpu_array_next];
   }

   cpu_array_next = (cpu_array_next + 1) % cpu_array_nr;
   return cpu;
}

void new_process_cb(struct task_struct* p, int clone) {
   int i;
   char comm[TASK_COMM_LEN];

   if(!p) {
      return;
   }

   get_task_comm(comm, p);

   //printk("%s has been called for pid: %d (name = %s)\n", clone ? "Clone" : "Execve", p->pid, comm);

   for(i = 0; i < num_considered_apps; i++) {
      char * app = considered_apps[i];
      if(strnstr(comm, app, TASK_COMM_LEN)) {
         break;
      }
   }

   if((!num_considered_apps && clone) || i < num_considered_apps) {
      struct cpumask dstp;
      int cpu = __get_next_cpu();

      cpumask_clear(&dstp);
      cpumask_set_cpu(cpu, &dstp);
      sched_setaffinity(p->pid, &dstp);

      printk("Assigning pid %d (%s) to core %d\n", p->pid,  comm, cpu);
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

   cpu_array_nr = num_present_cpus();
   cpu_array = kmalloc(cpu_array_nr * sizeof(int), GFP_KERNEL);

   if(!cpu_array) {
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
   
   clone_callback = &new_process_cb;
 
   return 0;
}

static void __exit pinthreads_exit_module(void) {
   clone_callback = NULL;
}

module_init(pinthreads_init_module);
module_exit(pinthreads_exit_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Fabien Gaud");
MODULE_DESCRIPTION("Kernel module to pin threads on cores");
