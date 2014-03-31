#ifndef __CARREFOUR_STATS__
#define __CARREFOUR_STATS__

#include <linux/percpu.h>

#define ENABLE_GLOBAL_STATS					1
#define ENABLE_MIGRATION_STATS				1
#define ENABLE_TSK_MIGRATION_STATS			1
#define ENABLE_TSK_MIGRATION_TIME_STATS	0 // WARNING: Should be checked -- especially for locking...
#define ENABLE_RWLOCK_STATS					0
#define ENABLE_MM_FUN_STATS					0

#define PROCFS_LOCK_FN "time_lock"
#define PROCFS_CARREFOUR_STATS_FN  "carrefour_replication_stats"

// WARNING: We use automerging for stats
// You MUST use only uint64_t types
typedef struct __attribute__((packed)) {
#if ENABLE_GLOBAL_STATS
   uint64_t nr_mm_switch;

   uint64_t nr_collapses;
   uint64_t nr_replicated_pages;
   uint64_t nr_ignored_orders;

   uint64_t nr_readlock_taken;
   uint64_t time_spent_acquiring_readlocks;
   uint64_t nr_writelock_taken;
   uint64_t time_spent_acquiring_writelocks;

   uint64_t time_spent_spinlocks;

   uint64_t nr_pgfault;
   uint64_t time_spent_in_pgfault_handler;
   uint64_t time_spent_in_pgfault_crit_sec;

   uint64_t nr_pingpong;
   uint64_t nr_replicated_decisions_reverted;
#endif

#if ENABLE_MIGRATION_STATS
   uint64_t migr_4k_from_to_node[MAX_NUMNODES][MAX_NUMNODES];
   uint64_t migr_2M_from_to_node[MAX_NUMNODES][MAX_NUMNODES];

   uint64_t nr_4k_pages_freed;
   uint64_t nr_4k_pages_migrated_at_least_once;
   uint64_t max_nr_migrations_per_4k_page;

   uint64_t nr_2M_pages_freed;
   uint64_t nr_2M_pages_migrated_at_least_once;
   uint64_t max_nr_migrations_per_2M_page;
#endif

   spinlock_t lock;
} replication_stats_t;

extern int start_carrefour_profiling;
extern rwlock_t reset_stats_rwl;

#if ENABLE_GLOBAL_STATS
DECLARE_PER_CPU(replication_stats_t, replication_stats_per_core);

#define RECORD_DURATION_START \
   unsigned long rdt_start, rdt_stop; \
   rdtscll(rdt_start)

#define RECORD_DURATION_START_VAL(rdt_start) rdtscll(rdt_start)

#define RECORD_DURATION_END(time_counter, acc_counter) \
   rdtscll(rdt_stop); \
   { \
      replication_stats_t* stats; \
      read_lock(&reset_stats_rwl); \
      stats = get_cpu_ptr(&replication_stats_per_core); \
      spin_lock(&stats->lock); \
      stats->acc_counter++; \
      stats->time_counter+= (rdt_stop - rdt_start); \
      spin_unlock(&stats->lock); \
      put_cpu_ptr(&replication_stats_per_core); \
      read_unlock(&reset_stats_rwl); \
   }

#define RECORD_DURATION_END_VAL(rdt_start, time_counter) \
   { \
      unsigned long rdt_stop; \
      replication_stats_t* stats; \
      \
      rdtscll(rdt_stop); \
      read_lock(&reset_stats_rwl); \
      stats = get_cpu_ptr(&replication_stats_per_core); \
      spin_lock(&stats->lock); \
      stats->time_counter+= (rdt_stop - rdt_start); \
      spin_unlock(&stats->lock); \
      put_cpu_ptr(&replication_stats_per_core); \
      read_unlock(&reset_stats_rwl); \
   }

#define INCR_REP_STAT_VALUE(entry, value) { \
   replication_stats_t* stats; \
   read_lock(&reset_stats_rwl); \
   stats = get_cpu_ptr(&replication_stats_per_core); \
   \
   spin_lock(&stats->lock); \
   stats->entry += (value); \
   spin_unlock(&stats->lock); \
   put_cpu_ptr(&replication_stats_per_core); \
   read_unlock(&reset_stats_rwl); \
}

#else // !ENABLE_GLOBAL_STATS
#define RECORD_DURATION_START					__attribute__((unused)) unsigned long rdt_start = 0, rdt_stop = 0; do {} while (0)
#define RECORD_DURATION_END(e, a)			do {} while (0)
#define RECORD_DURATION_START_VAL(e)		do {} while (0)
#define RECORD_DURATION_END_VAL(e, a)		do {} while (0)
#define INCR_REP_STAT_VALUE(e,v)				do {} while (0)
#endif

#if ENABLE_TSK_MIGRATION_STATS
typedef struct __attribute__((packed)) {
   uint64_t nr_tsk_migrations_idle;
   uint64_t nr_tsk_migrations_rebalance;
   uint64_t nr_tsk_migrations_wakeup;
   uint64_t nr_tsk_migrations_wakeup_new;

   uint64_t nr_tsk_migrations_others;

   uint64_t nr_tsk_migrations_in_rw_lock;
} tsk_migrations_stats_t;

DECLARE_PER_CPU(tsk_migrations_stats_t, tsk_migrations_stats_per_core);

// We don't use get_cpu_ptr, because we don't really care about preemption
#define INCR_TSKMIGR_STAT_VALUE(entry, value) { \
   if(likely(start_carrefour_profiling)){ \
      tsk_migrations_stats_t* stats; \
      stats = this_cpu_ptr(&tsk_migrations_stats_per_core); \
      \
      __sync_fetch_and_add(&stats->entry, (value)); \
   } \
}

#else
typedef int tsk_migrations_stats_t; // make sure that the name exists
#define INCR_TSKMIGR_STAT_VALUE(e,v) do {} while (0)
#endif

void record_fn_call(const char* fn_name, const char * suffix, unsigned long duration);

void start_profiling_hwc(void);
void stop_profiling(const char * fn_name, const char* suffix);

#if !ENABLE_GLOBAL_STATS && (ENABLE_MIGRATION_STATS || ENABLE_MM_LOCK_STATS || ENABLE_TSK_MIGRATION_STATS)
#error "Cannot enable ENABLE_MIGRATION_STATS or ENABLE_MM_LOCK_STATS or ENABLE_TSK_MIGRATION_STATS without ENABLE_GLOBAL_STATS"
#endif

#endif
