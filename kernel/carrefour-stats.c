#include <linux/carrefour-stats.h>
#include <linux/carrefour-hooks.h> //todo clean
#include <linux/carrefour-hooks.h> //todo clean
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/module.h>

#if ENABLE_GLOBAL_STATS

DEFINE_RWLOCK(reset_stats_rwl);
DEFINE_PER_CPU(replication_stats_t, replication_stats_per_core);

int start_carrefour_profiling = 0;

#if ENABLE_TSK_MIGRATION_STATS
DEFINE_PER_CPU(tsk_migrations_stats_t, tsk_migrations_stats_per_core);
#endif

/**
PROCFS Functions
We create here entries in the proc system that will allows us to configure replication and gather stats :)
That's not very clean, we should use sysfs instead [TODO]
**/
static u64 last_rdt = 0;
// Do not use something else than unsigned long !
struct time_profiling_t {
   unsigned long timelock;
   unsigned long timewlock;
   unsigned long timespinlock;
   unsigned long timemmap;
   unsigned long timemunmap;
   unsigned long timebrk;
   unsigned long timemprotect;
   unsigned long timepgflt;
};

static struct time_profiling_t last_time_prof;

static void _get_merged_lock_time(struct time_profiling_t* merged) {
   int cpu;

   memset(merged, 0, sizeof(struct time_profiling_t));

   /** Merging stats **/
   write_lock(&reset_stats_rwl);

   for_each_online_cpu(cpu) {
      replication_stats_t * stats = per_cpu_ptr(&replication_stats_per_core, cpu);

      merged->timelock += (stats->time_spent_acquiring_readlocks + stats->time_spent_acquiring_writelocks);
      merged->timewlock += (stats->time_spent_acquiring_writelocks);
      merged->timespinlock += (stats->time_spent_spinlocks);

#if ENABLE_MM_LOCK_STATS
      merged->timemmap += (stats->time_spent_mmap_lock);
      merged->timebrk += (stats->time_spent_brk_lock);
      merged->timemunmap += (stats->time_spent_munmap_lock);
      merged->timemprotect += (stats->time_spent_mprotect_lock);
#endif

      if(merged->timepgflt < stats->time_spent_in_pgfault_handler) {
         merged->timepgflt = (stats->time_spent_in_pgfault_handler);
      }
   }

   write_unlock(&reset_stats_rwl);
}

static int get_lock_contention(struct seq_file *m, void* v)
{
   unsigned long rdt;
   struct time_profiling_t current_time_prof, current_time_prof_acc;
   unsigned long div;
   int i;

   if(!last_rdt) {
      seq_printf(m, "You must write to the file first !\n");
      return 0;
   } 

   rdtscll(rdt);
   rdt -= last_rdt;

   _get_merged_lock_time(&current_time_prof_acc);

   // Auto merging
   for(i = 0; i < sizeof(struct time_profiling_t) / sizeof(unsigned long); i++) {
      ((unsigned long*) &current_time_prof)[i] = ((unsigned long *) &current_time_prof_acc)[i] - ((unsigned long *) &last_time_prof)[i];
   }

   // Save the current_time_prof
   memcpy(&last_time_prof, &current_time_prof_acc, sizeof(struct time_profiling_t));

   div = rdt * num_online_cpus();
   
   if(rdt) {
      u64 total_migr = 0;

      write_lock(&carrefour_hook_stats_lock);
      for(i = 0; i < num_online_cpus(); i++) {
         struct carrefour_migration_stats_t * stats = per_cpu_ptr(&carrefour_migration_stats, i);
         total_migr += stats->time_spent_in_migration_2M + stats->time_spent_in_migration_4k;
      }
      write_unlock(&carrefour_hook_stats_lock);

      seq_printf(m, "%lu %lu %lu %lu %lu %lu %lu %llu\n",
            (current_time_prof.timelock * 100) / div, (current_time_prof.timespinlock * 100) / div,
            (current_time_prof.timemmap * 100) / current_time_prof.timewlock, (current_time_prof.timebrk * 100) / current_time_prof.timewlock, 
            (current_time_prof.timemunmap * 100) / current_time_prof.timewlock, (current_time_prof.timemprotect * 100) / current_time_prof.timewlock,
            (current_time_prof.timepgflt * 100) / rdt,
            (total_migr * 100) / div
         );
   }

   rdtscll(last_rdt);
   return 0;
}

static void _lock_contention_reset(void) {
   rdtscll(last_rdt);
   _get_merged_lock_time(&last_time_prof);
}

static ssize_t lock_contention_reset(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
   _lock_contention_reset();
   return count;
}

static int lock_contention_open(struct inode *inode, struct file *file) {
   return single_open(file, get_lock_contention, NULL);
}

static const struct file_operations lock_handlers = {
   .owner   = THIS_MODULE,
   .open    = lock_contention_open,
   .read    = seq_read,
   .llseek  = seq_lseek,
   .release = seq_release,
   .write   = lock_contention_reset,
};


static int display_carrefour_stats(struct seq_file *m, void* v)
{
   replication_stats_t* global_stats;
   tsk_migrations_stats_t* global_tsk_stats;

   int cpu, i;
   unsigned long time_rd_lock	= 0;
   unsigned long time_wr_lock	= 0;
   unsigned long time_lock	= 0;
   unsigned long time_pgfault = 0;
   unsigned long time_pgfault_crit = 0;
   unsigned long max_time_pgflt = 0;
#if ENABLE_MIGRATION_STATS
   unsigned long nr_migrations = 0;
	int j;
#endif
#if ENABLE_MM_LOCK_STATS
   unsigned long max_time_mmap = 0;
   unsigned long max_time_munmap = 0;
   unsigned long avg1 = 0, avg2 = 0;
#endif
#if ENABLE_TSK_MIGRATION_STATS
	unsigned long total_nr_task_migrations = 0;
	int ratio = 0;
#endif

   seq_printf(m, "#Number of online cpus: %d\n", num_online_cpus());
   seq_printf(m, "#Number of online nodes: %d\n", num_online_nodes());

   /** Merging stats **/
   global_stats = kmalloc(sizeof(replication_stats_t), GFP_KERNEL | __GFP_ZERO);
   global_tsk_stats = kmalloc(sizeof(tsk_migrations_stats_t), GFP_KERNEL | __GFP_ZERO);
   if(!global_stats || !global_tsk_stats) {
      printk(KERN_CRIT "No more memory ?\n");
		BUG_ON(1);
   }

   write_lock(&reset_stats_rwl);

   for_each_online_cpu(cpu) {
      replication_stats_t * stats = per_cpu_ptr(&replication_stats_per_core, cpu);
#if ENABLE_TSK_MIGRATION_STATS
      tsk_migrations_stats_t* tsk_stats = per_cpu_ptr(&tsk_migrations_stats_per_core, cpu);
#endif

#if ENABLE_MM_LOCK_STATS
      unsigned long time_mmap_sum = 0;
      unsigned long time_munmap_sum = 0;
#endif

      uint64_t* stats_p = (uint64_t*) stats;

      // Automatic merging of everything
      for(i = 0; i < sizeof(replication_stats_t) / sizeof(uint64_t); i++) {
         if((&stats_p[i] == &stats->time_spent_in_pgfault_handler) && (stats_p[i] > max_time_pgflt)) {
            max_time_pgflt = stats_p[i];
         }
 
#if ENABLE_MIGRATION_STATS
         if(&stats_p[i] == &stats->max_nr_migrations_per_4k_page) {
            // We don't want to automerge this one
            continue;
         }
#endif

#if ENABLE_MM_LOCK_STATS
			if(&stats_p[i] == &stats->time_spent_mmap_lock) {
            time_mmap_sum += stats_p[i];
         }
         else if(&stats_p[i] == &stats->time_spent_mmap_crit_sec) {
            time_mmap_sum += stats_p[i];
         }
         else if(&stats_p[i] == &stats->time_spent_munmap_lock) {
            time_munmap_sum += stats_p[i];
         }
         else if(&stats_p[i] == &stats->time_spent_munmap_crit_sec) {
            time_munmap_sum += stats_p[i];
         }
#endif

         ((uint64_t *) global_stats)[i] += stats_p[i];
      }


#if ENABLE_TSK_MIGRATION_STATS
      // Automatic merging of everything
      stats_p = (uint64_t*) tsk_stats;
      for(i = 0; i < sizeof(tsk_migrations_stats_t) / sizeof(uint64_t); i++) {
         ((uint64_t *) global_tsk_stats)[i] += stats_p[i];

			if(&stats_p[i] != &tsk_stats->nr_tsk_migrations_in_mm_lock) {
				total_nr_task_migrations += stats_p[i];
			}
      }
#endif

#if ENABLE_MIGRATION_STATS
      if(stats->max_nr_migrations_per_4k_page > global_stats->max_nr_migrations_per_4k_page) {
         global_stats->max_nr_migrations_per_4k_page = stats->max_nr_migrations_per_4k_page;
      }
#endif

#if ENABLE_MM_LOCK_STATS 
      if(time_mmap_sum > max_time_mmap) {
         max_time_mmap = time_mmap_sum;
      }

      if(time_munmap_sum > max_time_munmap) {
         max_time_munmap = time_munmap_sum;
      }
#endif
   }

   write_unlock(&reset_stats_rwl);


   if(global_stats->nr_readlock_taken) {
      time_rd_lock = (unsigned long) (global_stats->time_spent_acquiring_readlocks / global_stats->nr_readlock_taken);
   }
   if(global_stats->nr_writelock_taken) {
      time_wr_lock = (unsigned long) (global_stats->time_spent_acquiring_writelocks / global_stats->nr_writelock_taken);
   }
   if(global_stats->nr_readlock_taken + global_stats->nr_writelock_taken) {
      time_lock = (unsigned long) ((global_stats->time_spent_acquiring_readlocks + global_stats->time_spent_acquiring_writelocks) / (global_stats->nr_readlock_taken + global_stats->nr_writelock_taken));
   }
   if(global_stats->nr_pgfault) {
      time_pgfault = (unsigned long) (global_stats->time_spent_in_pgfault_handler / global_stats->nr_pgfault);
      time_pgfault_crit = (unsigned long) (global_stats->time_spent_in_pgfault_crit_sec / global_stats->nr_pgfault);
   }

   seq_printf(m, "[GLOBAL] Number of MM switch: %lu\n", (unsigned long) global_stats->nr_mm_switch);
   seq_printf(m, "[GLOBAL] Number of collapses: %lu\n", (unsigned long) global_stats->nr_collapses);
   seq_printf(m, "[GLOBAL] Number of ping pongs: %lu\n", (unsigned long) global_stats->nr_pingpong);
   seq_printf(m, "[GLOBAL] Number of reverted replication decisions: %lu\n", (unsigned long) global_stats->nr_replicated_decisions_reverted);
   seq_printf(m, "[GLOBAL] Number of replicated pages: %lu\n", (unsigned long) global_stats->nr_replicated_pages);
   seq_printf(m, "[GLOBAL] Number of ignored orders: %lu\n\n", (unsigned long) global_stats->nr_ignored_orders);

   seq_printf(m, "[GLOBAL] Time spent acquiring read locks: %lu cycles\n", time_rd_lock);
   seq_printf(m, "[GLOBAL] Time spent acquiring write locks: %lu cycles\n", time_wr_lock);
   seq_printf(m, "[GLOBAL] Time spent acquiring locks (global): %lu cycles\n\n", time_lock);
   
   seq_printf(m, "[GLOBAL] Time spent acquiring spinlocks (total, global): %lu cycles\n", (unsigned long) global_stats->time_spent_spinlocks);

#if ENABLE_MM_LOCK_STATS
   if(global_stats->nr_mmap) {
      avg1 = global_stats->time_spent_mmap_lock / global_stats->nr_mmap;
      avg2 = global_stats->time_spent_mmap_crit_sec / global_stats->nr_mmap;
   }
   seq_printf(m, "[GLOBAL] Time spent in mmap (lock, crit. section): %lu cycles, %lu cycles (%lu cycles, %lu cycles, %lu cycles)\n",
      avg1, avg2, (unsigned long) global_stats->time_spent_mmap_lock, (unsigned long) global_stats->time_spent_mmap_crit_sec, max_time_mmap);

   avg1 = avg2 = 0;
   if(global_stats->nr_munmap) {
      avg1 = global_stats->time_spent_munmap_lock / global_stats->nr_munmap;
      avg2 = global_stats->time_spent_munmap_crit_sec / global_stats->nr_munmap;
   }
   seq_printf(m, "[GLOBAL] Time spent in munmap (lock, crit. section): %lu cycles, %lu cycles (%lu cycles, %lu cycles, %lu cycles)\n",
      avg1, avg2, (unsigned long) global_stats->time_spent_munmap_lock, (unsigned long) global_stats->time_spent_munmap_crit_sec, max_time_munmap);

   avg1 = avg2 = 0;
   if(global_stats->nr_mprotect) {
      avg1 = global_stats->time_spent_mprotect_lock / global_stats->nr_mprotect;
      //avg2 = global_stats->time_spent_mprotect_crit_sec / global_stats->nr_mprotect;
   }
   seq_printf(m, "[GLOBAL] Time spent in mprotect (lock, crit. section): %lu cycles, %lu cycles (%lu cycles, %lu cycles, %lu cycles)\n",
      avg1, avg2, (unsigned long) global_stats->time_spent_mprotect_lock, (unsigned long) 0, (unsigned long) 0);

   avg1 = avg2 = 0;
   if(global_stats->nr_brk) {
      avg1 = global_stats->time_spent_brk_lock / global_stats->nr_brk;
      //avg2 = global_stats->time_spent_brk_crit_sec / global_stats->nr_brk;
   }
   seq_printf(m, "[GLOBAL] Time spent in brk (lock, crit. section): %lu cycles, %lu cycles (%lu cycles, %lu cycles, %lu cycles)\n",
      avg1, avg2, (unsigned long) global_stats->time_spent_brk_lock, (unsigned long) 0, (unsigned long) 0);
#endif

   seq_printf(m, "[GLOBAL] Number of page faults: %lu\n", (unsigned long) global_stats->nr_pgfault);
   seq_printf(m, "[GLOBAL] Time spent in the page fault handler: %lu cycles\n\n", time_pgfault);
   seq_printf(m, "[GLOBAL] Time spent in the page fault handler (not including mm lock): %lu cycles\n\n", time_pgfault_crit);
   seq_printf(m, "[GLOBAL] Max time spent in the page fault handler: %lu cycles (total on one core)\n\n", max_time_pgflt);

#if ENABLE_MIGRATION_STATS
   seq_printf(m, "[GLOBAL] 4k pages:\n");
   seq_printf(m, "[GLOBAL] Number of pages freed (i.e, approx. total number of pages): %lu\n", (unsigned long) global_stats->nr_4k_pages_freed);
   seq_printf(m, "[GLOBAL] Number of pages migrated at least once: %lu\n", (unsigned long) global_stats->nr_4k_pages_migrated_at_least_once);
   seq_printf(m, "[GLOBAL] Max number of migrations per page: %lu\n", (unsigned long) global_stats->max_nr_migrations_per_4k_page);

   for(i = 0; i < num_online_nodes(); i++) {
      seq_printf(m, "[GLOBAL] Moved pages from node %d: ", i);
      for(j = 0; j < num_online_nodes(); j++) {
         seq_printf(m,"%lu\t", (unsigned long) global_stats->migr_4k_from_to_node[i][j]);

         nr_migrations += global_stats->migr_4k_from_to_node[i][j];
      }
      seq_printf(m, "\n");
   }
   seq_printf(m, "[GLOBAL] Number of migrations: %lu\n\n", nr_migrations);

   seq_printf(m, "[GLOBAL] 2M pages:\n");
   seq_printf(m, "[GLOBAL] Number of pages freed (i.e, approx. total number of pages): %lu\n", (unsigned long) global_stats->nr_2M_pages_freed);
   seq_printf(m, "[GLOBAL] Number of pages migrated at least once: %lu\n", (unsigned long) global_stats->nr_2M_pages_migrated_at_least_once);
   seq_printf(m, "[GLOBAL] Max number of migrations per page: %lu\n", (unsigned long) global_stats->max_nr_migrations_per_2M_page);

   nr_migrations = 0;
   for(i = 0; i < num_online_nodes(); i++) {
      seq_printf(m, "[GLOBAL] Moved pages from node %d: ", i);
      for(j = 0; j < num_online_nodes(); j++) {
         seq_printf(m,"%lu\t", (unsigned long) global_stats->migr_2M_from_to_node[i][j]);

         nr_migrations += global_stats->migr_2M_from_to_node[i][j];
      }
      seq_printf(m, "\n");
   }
   seq_printf(m, "[GLOBAL] Number of migrations: %lu\n\n", nr_migrations);
#endif

#if ENABLE_TSK_MIGRATION_STATS
	ratio = total_nr_task_migrations ? global_tsk_stats->nr_tsk_migrations_idle * 100 / total_nr_task_migrations : 0; 
   seq_printf(m, "[GLOBAL] Number of task migrations due to load balance (idle): %lu (%d %%)\n", (unsigned long) global_tsk_stats->nr_tsk_migrations_idle, ratio);

	ratio = total_nr_task_migrations ? global_tsk_stats->nr_tsk_migrations_rebalance * 100 / total_nr_task_migrations : 0; 
   seq_printf(m, "[GLOBAL] Number of task migrations due to load balance (rebalance): %lu (%d %%)\n", (unsigned long) global_tsk_stats->nr_tsk_migrations_rebalance, ratio);

	ratio = total_nr_task_migrations ? global_tsk_stats->nr_tsk_migrations_wakeup * 100 / total_nr_task_migrations : 0; 
   seq_printf(m, "[GLOBAL] Number of task migrations due to wake up: %lu (%d %%)\n", (unsigned long) global_tsk_stats->nr_tsk_migrations_wakeup, ratio);

	ratio = total_nr_task_migrations ? global_tsk_stats->nr_tsk_migrations_wakeup_new * 100 / total_nr_task_migrations : 0; 
   seq_printf(m, "[GLOBAL] Number of task migrations due to wake up (new): %lu (%d %%)\n", (unsigned long) global_tsk_stats->nr_tsk_migrations_wakeup_new, ratio);

	ratio = total_nr_task_migrations ? global_tsk_stats->nr_tsk_migrations_others * 100 / total_nr_task_migrations : 0; 
   seq_printf(m, "[GLOBAL] Number of task migrations due to others: %lu (%d %%)\n", (unsigned long) global_tsk_stats->nr_tsk_migrations_others, ratio);

	ratio = total_nr_task_migrations ? global_tsk_stats->nr_tsk_migrations_in_mm_lock * 100 / total_nr_task_migrations : 0; 
   seq_printf(m, "[GLOBAL] Number of task migrations while mmap/munmap in mm lock: %lu (%d %%)\n", (unsigned long) global_tsk_stats->nr_tsk_migrations_in_mm_lock, ratio);
#endif

   kfree(global_stats);
	kfree(global_tsk_stats);

   return 0;
}

static int carrefour_stats_open(struct inode *inode, struct file *file) {
   return single_open(file, display_carrefour_stats, NULL);
}

static ssize_t carrefour_stats_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
   int cpu;

   write_lock(&reset_stats_rwl);
   for_each_online_cpu(cpu) {
      /** Don't need to disable preemption here because we have the write lock **/
      replication_stats_t * stats = per_cpu_ptr(&replication_stats_per_core, cpu);
#if ENABLE_TSK_MIGRATION_STATS
      tsk_migrations_stats_t * stats_tsk = per_cpu_ptr(&tsk_migrations_stats_per_core, cpu);
      memset(stats_tsk, 0, sizeof(tsk_migrations_stats_t));
#endif
      memset(stats, 0, sizeof(replication_stats_t));
   }

   write_unlock(&reset_stats_rwl);

   _lock_contention_reset();
   return count;
}

static const struct file_operations carrefour_stats_handlers = {
   .owner   = THIS_MODULE,
   .open    = carrefour_stats_open,
   .read    = seq_read,
   .llseek  = seq_lseek,
   .release = seq_release,
   .write   = carrefour_stats_write,
};

static int __init carrefour_stats_init(void)
{
   int cpu;
   for_each_online_cpu(cpu) {
      /** We haven't disable premption here but I think that's not a big deal because it's during the initalization **/
      replication_stats_t * stats = per_cpu_ptr(&replication_stats_per_core, cpu);
#if ENABLE_TSK_MIGRATION_STATS
      tsk_migrations_stats_t * stats_tsk = per_cpu_ptr(&tsk_migrations_stats_per_core, cpu);
      memset(stats_tsk, 0, sizeof(tsk_migrations_stats_t));
#endif
      memset(stats, 0, sizeof(replication_stats_t));
   }

   if(!proc_create(PROCFS_CARREFOUR_STATS_FN, S_IRUGO, NULL, &carrefour_stats_handlers)){
      printk(KERN_ERR "Cannot create /proc/%s\n", PROCFS_CARREFOUR_STATS_FN);
      return -ENOMEM;
   }

   if(!proc_create(PROCFS_LOCK_FN, S_IRUGO, NULL, &lock_handlers)){
		printk(KERN_ERR "Cannot create /proc/%s\n", PROCFS_LOCK_FN);
      return -ENOMEM;
   }

   start_carrefour_profiling = 1;

	return 0;
}

module_init(carrefour_stats_init)
#endif
