#include <linux/carrefour-stats.h>
#include <linux/carrefour-hooks.h> //todo clean
#include <linux/carrefour-hooks.h> //todo clean
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/module.h>

#include <linux/sort.h>

#include <linux/perf_event.h>
#include <linux/kthread.h>

#if ENABLE_GLOBAL_STATS

DEFINE_RWLOCK(reset_stats_rwl);
DEFINE_PER_CPU(replication_stats_t, replication_stats_per_core);

int start_carrefour_profiling = 0;
u64 last_rdt_carrefour_stats = 0;

#if ENABLE_TSK_MIGRATION_STATS
DEFINE_PER_CPU(tsk_migrations_stats_t, tsk_migrations_stats_per_core);
#endif

#define MAX_FUNCTIONS		100
#define MAX_FN_NAME_LENGTH 30

#if ENABLE_HWC_PROFILING
#define MAX_NR_HWC			4

//static unsigned long events_config[] = {0x00040ff41, 0x000400081, 0x00040FF7E, 0x40040f7E1}; // L1D misses, L1I misses, L2 misses, L3 misses 
static unsigned long events_config[] = {0x00040ff41, 0x000400081, 0x00040FF7E}; // L1D misses, L1I misses, L2 misses
static unsigned long events_warned[] = {0};

struct hwc_prof_per_task {
	struct perf_event * event;

	u64 initial_value;
	u64 initial_enabled;
	u64 initial_running;
	u64 skipped_not_running;
};

// Watchdog
static DEFINE_PER_CPU(struct task_struct *, softlockup_watchdog);
static int watchdog_disabled = 0;
#endif

enum stats_type {TIME, HWC};
struct fn_stats_t {
	struct rb_node node;

	char name[MAX_FN_NAME_LENGTH];
	enum stats_type type;

	union {
		struct {
			unsigned long time_spent;
			unsigned long nr_calls;
			unsigned long max_time_spent_per_core;
		};
#if ENABLE_HWC_PROFILING
		struct {
			unsigned long nr_calls;
			unsigned long hwc_value[MAX_NR_HWC];
			unsigned	nr_hwc;
		};
#endif
	};
};

struct fn_stats_tree_t {
	struct rb_root root;

	struct fn_stats_t fn_entries[MAX_FUNCTIONS];
	int index;
};

DEFINE_PER_CPU(struct fn_stats_tree_t, fn_stats_tree_per_cpu);
DEFINE_PER_CPU(struct fn_stats_tree_t, fn_hwc_stats_tree_per_cpu);

struct fn_stats_t* find_fn_in_tree(struct fn_stats_tree_t* tree, const char* fn_name) {
   struct rb_node **new = &(tree->root.rb_node), *parent = NULL;
   struct fn_stats_t* f = NULL;

   /* Figure out where to put new node */
   while (*new) {
      struct fn_stats_t *this = container_of(*new, struct fn_stats_t, node);
      parent = *new;

      if (strcmp(fn_name, this->name) < 0) {
         new = &((*new)->rb_left);
      }
      else if (strcmp(fn_name, this->name) > 0) {
         new = &((*new)->rb_right);
      }
      else {
         return this;
      }
   }

   /* Add new node and rebalance tree. */
   if(tree->index < MAX_FUNCTIONS) {
      f = &tree->fn_entries[tree->index++];
      strncpy(f->name, fn_name, MAX_FN_NAME_LENGTH);
      f->name[MAX_FN_NAME_LENGTH-1] = 0;

      rb_link_node(&f->node, parent, new); 
      rb_insert_color(&f->node, &tree->root);
   }
   else {
      printk("Warning, not enough space in tree. Consider increasing MAX_FUNCTIONS\n");
   }

   return f;
}

void merge_fn_arrays(struct fn_stats_tree_t* dest, struct fn_stats_tree_t* src) {
	int i;
	for(i = 0; i < src->index; i++) {
		struct fn_stats_t* f;

		f = find_fn_in_tree(dest, src->fn_entries[i].name);

		if(f){
			f->type = src->fn_entries[i].type;

			if(f->type == TIME) {
				f->nr_calls += src->fn_entries[i].nr_calls;
				f->time_spent += src->fn_entries[i].time_spent;

				if(src->fn_entries[i].time_spent > f->max_time_spent_per_core) {
					f->max_time_spent_per_core = src->fn_entries[i].time_spent;
				}
			}
#if ENABLE_HWC_PROFILING
			else {
				int j;
				f->nr_hwc = src->fn_entries[i].nr_hwc;

				f->nr_calls += src->fn_entries[i].nr_calls;
				for (j = 0; j < src->fn_entries[i].nr_hwc; j++) {
					f->hwc_value[j] += src->fn_entries[i].hwc_value[j];
				}
			}
#endif
		}
		else {
			printk("Warning, not enough space in merge tree. Consider increasing MAX_FUNCTIONS\n");
		}
	}
}

void record_fn_call(const char* fn_name, const char* suffix, unsigned long duration) {
	struct fn_stats_t* f;
	struct fn_stats_tree_t* tree = get_cpu_ptr(&fn_stats_tree_per_cpu);

	char name[MAX_FN_NAME_LENGTH];

	if(!start_carrefour_profiling) {
		goto exit;
	}

	if(suffix) {
		snprintf(name, MAX_FN_NAME_LENGTH, "%s%s", fn_name, suffix);
		fn_name = name;
	}

	f = find_fn_in_tree(tree, fn_name);
	if(f) {
		f->type = TIME;
		f->nr_calls++;
		f->time_spent += duration;
	}

exit:
	put_cpu_ptr(&fn_stats_tree_per_cpu);
}

#if ENABLE_HWC_PROFILING
static void record_fn_hwc(const char* fn_name, const char* suffix, int index, unsigned long value, unsigned nr_hwc) {
	struct fn_stats_t* f;
	struct fn_stats_tree_t* tree = get_cpu_ptr(&fn_hwc_stats_tree_per_cpu);
	char name[MAX_FN_NAME_LENGTH];

	if(!start_carrefour_profiling) {
		goto exit;	
	}

	if(suffix) {
		snprintf(name, MAX_FN_NAME_LENGTH, "%s%s", fn_name, suffix);
		fn_name = name;
	}

	f = find_fn_in_tree(tree, fn_name);
	if(f) {
		f->type = HWC;
		f->nr_hwc = nr_hwc;
		f->nr_calls++;
		f->hwc_value[index] += value;
	}	

exit:
	put_cpu_ptr(&fn_hwc_stats_tree_per_cpu);
}


static void hwc_overflow_handler(struct perf_event *event, struct perf_sample_data *data, struct pt_regs *regs) {
	printk("[BUG] Overflows not supported !!\n");
	BUG();
}

static void watchdog_disable_all_cpus(void) {
	unsigned int cpu;

	if (!watchdog_disabled) {
		for_each_online_cpu(cpu) {
			struct task_struct *k = per_cpu(softlockup_watchdog, cpu);

			if(k) {		
				kthread_park(k);
				watchdog_disabled = 1;
			}
			else {
				watchdog_disabled = 0;
				break;
			}
		}
	}
}

void init_hwc_prof(void) {
	struct hwc_prof_per_task* events;
	int i;
	int nb_events = sizeof(events_config) / sizeof(events_config[0]);

	if(!start_carrefour_profiling) {
		return;
	}

	if(unlikely(nb_events > MAX_NR_HWC)) {
		printk(KERN_CRIT "Big bug !\n");
		return;
	}

	events = kzalloc(sizeof(struct hwc_prof_per_task) * nb_events, GFP_KERNEL);
	if(!events) {
		printk("[BUG] No more memory ?\n");
		BUG();
	}

	current->private_data = (void*) events;

	for(i = 0; i < nb_events; i++) {
		struct perf_event_attr attr = {
			.type           = PERF_TYPE_RAW,
			.config         = events_config[i],
			.size           = sizeof(struct perf_event_attr),
			.exclude_kernel = 0,
			.exclude_user   = 0,
		};

		events[i].event = perf_event_create_kernel_counter(&attr, -1, current, hwc_overflow_handler, NULL);
		if (IS_ERR(events[i].event)) {
			if(!events_warned[i]) {
				events_warned[i] = 1; // No need to use atomic ops. We don't care if it is printed a few times
				printk(KERN_CRIT "BUG (ERR = %ld) -- index = %d\n", PTR_ERR(events[i].event), i);
			}
			events[i].event = NULL;
			continue;
		}

		perf_event_enable(events[i].event);
	}
}

void exit_hwc_prof(void) {
	struct hwc_prof_per_task * events = (struct hwc_prof_per_task *) current->private_data;
	int i;
	int nb_events = sizeof(events_config) / sizeof(events_config[0]);

	if(!start_carrefour_profiling ||  !events) {
		return;
	}

	for(i = 0; i < nb_events; i++) {
		if(!events[i].event) {
			// Initialization has failed
			continue;
		}

		perf_event_disable(events[i].event);
		perf_event_release_kernel(events[i].event);
	}

	if(events->skipped_not_running) {
		printk("PID %d -- skipped %lu recording\n", current->pid, events->skipped_not_running);
	}

	kfree(events);
	current->private_data = NULL;
}

void start_recording_hwc(void) {
	struct hwc_prof_per_task* events = (struct hwc_prof_per_task *) current->private_data;

	int i;
	int nb_events = sizeof(events_config) / sizeof(events_config[0]);

	if(!start_carrefour_profiling || !events) {
		return;
	}

	for(i = 0; i < nb_events; i++) {
		if(!events[i].event) {
			continue; // Initialization has failed
		}

		events[i].initial_value = perf_event_read_value(events[i].event, &events[i].initial_enabled, &events[i].initial_running); // TODO Support multiplexing properly
	}
}

void stop_recording_hwc(const char * fn_name, const char* suffix) {
	struct hwc_prof_per_task * events = (struct hwc_prof_per_task *) current->private_data;
	int i;
	int nb_events = sizeof(events_config) / sizeof(events_config[0]);

	if(!start_carrefour_profiling || !events) {
		return;
	}

	for(i = 0; i < nb_events; i++) {
		u64 enabled, running, value;

		if(!events[i].event) {
			continue; // Initialization has failed
		}

		value = perf_event_read_value(events[i].event, &enabled, &running);

		if((running - events[i].initial_running) > 0) {
			// Make sure that we don't register a call if the HWC has not been running
			// TODO: do it better ?
			record_fn_hwc(fn_name, suffix, i, (value - events[i].initial_value), nb_events);
		}
		else {
			events->skipped_not_running++;
		}
	}
}
#else
#define watchdog_disable_all_cpus() do {} while(0)
#endif

static int cmp_time (const void * a, const void * b) {
   struct fn_stats_t * fn_a = (struct fn_stats_t *) a;
   struct fn_stats_t * fn_b = (struct fn_stats_t *) b;

   // We cannot simply return ( fn_b->time_spent - fn_a->time_spent )
	// because these are 64 bit integers and we return an 'int' so there might be overflows
	if(fn_a->time_spent > fn_b->time_spent) {
		return -1;
	}
	
	if(fn_a->time_spent < fn_b->time_spent) {
		return 1;
	}

	return 0;
}

static void sort_times(struct fn_stats_tree_t* tree) {
   sort(tree->fn_entries, tree->index, sizeof(struct fn_stats_t), cmp_time, NULL); 
}

void fn_tree_print(struct seq_file *m, struct fn_stats_tree_t* tree, unsigned long duration) {
	int i;
	for(i = 0; i < tree->index; i++){
		if(tree->fn_entries[i].type == TIME) {
			int ratio = duration ? (tree->fn_entries[i].time_spent * 100 / (duration * num_online_cpus())): 0;
			int ratio_per_core = duration ? (tree->fn_entries[i].max_time_spent_per_core * 100 / duration): 0;
			unsigned long per_call = tree->fn_entries[i].nr_calls ? (tree->fn_entries[i].time_spent/tree->fn_entries[i].nr_calls): 0;
			seq_printf(m, "%*s -- nr calls %15lu -- time spent %20lu -- per call %13lu -- %2d %% -- max per core %2d %%\n",
					MAX_FN_NAME_LENGTH, tree->fn_entries[i].name, tree->fn_entries[i].nr_calls, tree->fn_entries[i].time_spent, per_call, ratio, ratio_per_core);
		}
#if ENABLE_HWC_PROFILING
		else {
			int j;

			seq_printf(m, "%*s -- %10lu -- ", MAX_FN_NAME_LENGTH, tree->fn_entries[i].name, tree->fn_entries[i].nr_calls);

			for(j = 0; j < tree->fn_entries[i].nr_hwc; j++) {
				unsigned long per_call = tree->fn_entries[i].nr_calls ? (tree->fn_entries[i].hwc_value[j]/tree->fn_entries[i].nr_calls): 0;

				seq_printf(m, " ( %15lu , %10lu )", tree->fn_entries[i].hwc_value[j], per_call);
			}
		}
#endif
	}
}

void __fn_tree_init(struct fn_stats_tree_t* tree) {
	memset(tree, 0, sizeof(struct fn_stats_tree_t));
	tree->root = RB_ROOT;
}

void fn_tree_init_safe(void) {
	int cpu;

	for_each_online_cpu(cpu) {
		struct fn_stats_tree_t* tree = per_cpu_ptr(&fn_stats_tree_per_cpu, cpu);
		struct fn_stats_tree_t* tree_hwc = per_cpu_ptr(&fn_hwc_stats_tree_per_cpu, cpu);

		__fn_tree_init(tree);
		__fn_tree_init(tree_hwc);
	}
}

/**
PROCFS Functions
We create here entries in the proc system that will allows us to configure replication and gather stats :)
That's not very clean, we should use sysfs instead [TODO]
**/
static u64 last_rdt_lock_contention = 0;
// Do not use something else than unsigned long !
struct time_profiling_t {
   unsigned long timelock;
   unsigned long timewlock;
   unsigned long timespinlock;
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

   if(!last_rdt_lock_contention) {
      seq_printf(m, "You must write to the file first !\n");
      return 0;
   } 

   rdtscll(rdt);
   rdt -= last_rdt_lock_contention;

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

      seq_printf(m, "%lu %lu %d %d %d %d %lu %llu\n",
            (current_time_prof.timelock * 100) / div, (current_time_prof.timespinlock * 100) / div,
            0, 0, 0, 0, // We keep it for compatibility reasons 
            (current_time_prof.timepgflt * 100) / rdt,
            (total_migr * 100) / div
         );
   }

   rdtscll(last_rdt_lock_contention);
   return 0;
}

static void _lock_contention_reset(void) {
   rdtscll(last_rdt_lock_contention);
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
#if ENABLE_TSK_MIGRATION_STATS
	unsigned long total_nr_task_migrations = 0;
	int ratio = 0;
#endif
	unsigned long rdt = 0;
	struct fn_stats_tree_t* dest_fn_stats_tree; 
	struct fn_stats_tree_t* dest_fn_hwc_stats_tree; 

	if(last_rdt_carrefour_stats) {
		rdtscll(rdt);
		rdt -= last_rdt_carrefour_stats;
	}

	dest_fn_stats_tree = kmalloc(sizeof(struct fn_stats_tree_t), GFP_KERNEL);
	dest_fn_hwc_stats_tree = kmalloc(sizeof(struct fn_stats_tree_t), GFP_KERNEL);
	if(!dest_fn_stats_tree || !dest_fn_hwc_stats_tree) {
		printk(KERN_CRIT "No more memory ?\n");
		BUG_ON(1);
	}

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

         ((uint64_t *) global_stats)[i] += stats_p[i];
      }

#if ENABLE_MIGRATION_STATS
      if(stats->max_nr_migrations_per_4k_page > global_stats->max_nr_migrations_per_4k_page) {
         global_stats->max_nr_migrations_per_4k_page = stats->max_nr_migrations_per_4k_page;
      }
#endif
   }
   write_unlock(&reset_stats_rwl);

#if ENABLE_TSK_MIGRATION_STATS
   for_each_online_cpu(cpu) {
      tsk_migrations_stats_t* tsk_stats;
      uint64_t* stats_p;

		// We go on each cpu so we don't have to take any lock.
		sched_setaffinity(0, get_cpu_mask(cpu));

		tsk_stats = get_cpu_ptr(&tsk_migrations_stats_per_core);

      // Automatic merging of everything
      stats_p = (uint64_t*) tsk_stats;
      for(i = 0; i < sizeof(tsk_migrations_stats_t) / sizeof(uint64_t); i++) {
         ((uint64_t *) global_tsk_stats)[i] += stats_p[i];

			if(&stats_p[i] < &tsk_stats->nr_tsk_migrations_in_rw_lock) {
				total_nr_task_migrations += stats_p[i];
			}
      }
   }
#endif

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
   seq_printf(m, "[GLOBAL] Time spent acquiring locks (global): %lu cycles\n", time_lock);
   seq_printf(m, "[GLOBAL] Nr of read locks taken: %lu\n", (unsigned long) global_stats->nr_readlock_taken);
   seq_printf(m, "[GLOBAL] Nr of write locks taken: %lu\n\n", (unsigned long) global_stats->nr_writelock_taken);
   
   seq_printf(m, "[GLOBAL] Time spent acquiring spinlocks (total, global): %lu cycles\n", (unsigned long) global_stats->time_spent_spinlocks);

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

	ratio = total_nr_task_migrations ? global_tsk_stats->nr_tsk_migrations_active_lb_cpu_stop * 100 / total_nr_task_migrations : 0; 
   seq_printf(m, "[GLOBAL] Number of task migrations due to active cpu stop: %lu (%d %%)\n", (unsigned long) global_tsk_stats->nr_tsk_migrations_active_lb_cpu_stop, ratio);

	ratio = total_nr_task_migrations ? global_tsk_stats->nr_tsk_migrations_others * 100 / total_nr_task_migrations : 0; 
   seq_printf(m, "[GLOBAL] Number of task migrations due to others: %lu (%d %%)\n", (unsigned long) global_tsk_stats->nr_tsk_migrations_others, ratio);

	ratio = total_nr_task_migrations ? global_tsk_stats->nr_tsk_migrations_in_rw_lock * 100 / total_nr_task_migrations : 0; 
   seq_printf(m, "[GLOBAL] Number of task migrations while task holding a rw lock: %lu (%d %%)\n\n", (unsigned long) global_tsk_stats->nr_tsk_migrations_in_rw_lock, ratio);


   seq_printf(m, "[GLOBAL] Total number of task migrations: %lu\n", (unsigned long) total_nr_task_migrations);

	ratio = total_nr_task_migrations ? global_tsk_stats->nr_tsk_migration_to_local_node * 100 / total_nr_task_migrations : 0;
   seq_printf(m, "[GLOBAL] Total number of task migrations to local node: %lu (%d %%)\n", (unsigned long) global_tsk_stats->nr_tsk_migration_to_local_node, ratio);

	ratio = total_nr_task_migrations ? global_tsk_stats->nr_tsk_migration_to_remote_node * 100 / total_nr_task_migrations : 0;
   seq_printf(m, "[GLOBAL] Total number of task migrations to remote node: %lu (%d %%)\n", (unsigned long) global_tsk_stats->nr_tsk_migration_to_remote_node, ratio);

	ratio = total_nr_task_migrations ? global_tsk_stats->nr_tsk_migration_to_same_core * 100 / total_nr_task_migrations : 0;
   seq_printf(m, "[GLOBAL] Total number of task migrations to same core ...: %lu (%d %%)\n\n", (unsigned long) global_tsk_stats->nr_tsk_migration_to_same_core, ratio);
#endif

   seq_printf(m, "[GLOBAL] Estimated number of cycles: %lu\n\n", (unsigned long) rdt);

	//
	__fn_tree_init(dest_fn_stats_tree);
	__fn_tree_init(dest_fn_hwc_stats_tree);

	for_each_online_cpu(cpu) {
		struct fn_stats_tree_t* tree;
		struct fn_stats_tree_t* tree_hwc;

		// We go on each cpu so we don't have to take any lock.
		sched_setaffinity(0, get_cpu_mask(cpu));

		tree = get_cpu_ptr(&fn_stats_tree_per_cpu);
		tree_hwc = get_cpu_ptr(&fn_hwc_stats_tree_per_cpu);

		merge_fn_arrays(dest_fn_stats_tree, tree);
		merge_fn_arrays(dest_fn_hwc_stats_tree, tree_hwc);

		put_cpu_ptr(&fn_hwc_stats_tree_per_cpu);
		put_cpu_ptr(&fn_stats_tree_per_cpu);
	}

	sort_times(dest_fn_stats_tree);
	fn_tree_print(m, dest_fn_stats_tree, rdt);
	fn_tree_print(m, dest_fn_hwc_stats_tree, rdt);
	//

	kfree(dest_fn_stats_tree);
	kfree(dest_fn_hwc_stats_tree);
   kfree(global_stats);
	kfree(global_tsk_stats);

   return 0;
}

static int carrefour_stats_open(struct inode *inode, struct file *file) {
   return single_open(file, display_carrefour_stats, NULL);
}

static ssize_t carrefour_stats_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
   int cpu;

	watchdog_disable_all_cpus();

   write_lock(&reset_stats_rwl);
   for_each_online_cpu(cpu) {
      /** Don't need to disable preemption here because we have the write lock **/
      replication_stats_t * stats = per_cpu_ptr(&replication_stats_per_core, cpu);
      memset(stats, 0, sizeof(replication_stats_t));
   }
   write_unlock(&reset_stats_rwl);

	for_each_online_cpu(cpu) {
		struct fn_stats_tree_t* tree;
		struct fn_stats_tree_t* tree_hwc;

		// We go on each cpu so we don't have to take any lock.
		sched_setaffinity(0, get_cpu_mask(cpu));

		tree = get_cpu_ptr(&fn_stats_tree_per_cpu);
		tree_hwc = get_cpu_ptr(&fn_hwc_stats_tree_per_cpu);

		__fn_tree_init(tree);
		__fn_tree_init(tree_hwc);

		if(ENABLE_TSK_MIGRATION_STATS) {
			tsk_migrations_stats_t * stats_tsk = get_cpu_ptr(&tsk_migrations_stats_per_core);
			memset(stats_tsk, 0, sizeof(tsk_migrations_stats_t));
			put_cpu_ptr(&tsk_migrations_stats_per_core);
		}
 
		put_cpu_ptr(&fn_hwc_stats_tree_per_cpu);
		put_cpu_ptr(&fn_stats_tree_per_cpu);
	}

   _lock_contention_reset();

	rdtscll(last_rdt_carrefour_stats);
   start_carrefour_profiling = 1;

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

	fn_tree_init_safe();

   if(!proc_create(PROCFS_CARREFOUR_STATS_FN, S_IRUGO, NULL, &carrefour_stats_handlers)){
      printk(KERN_ERR "Cannot create /proc/%s\n", PROCFS_CARREFOUR_STATS_FN);
      return -ENOMEM;
   }

   if(!proc_create(PROCFS_LOCK_FN, S_IRUGO, NULL, &lock_handlers)){
		printk(KERN_ERR "Cannot create /proc/%s\n", PROCFS_LOCK_FN);
      return -ENOMEM;
   }

	return 0;
}

module_init(carrefour_stats_init)
#endif
