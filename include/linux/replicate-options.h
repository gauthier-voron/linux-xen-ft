#ifndef __REPLICATE_OPTIONS__
#define __REPLICATE_OPTIONS__

/** Configuration of replication internal stuff **/
// Look at include/linux/replicate.h for the headers

#define VERBOSE_PGFAULT                0
#define VERBOSE_REPTHREAD              0
#define VERBOSE_OTHERS                 0

#define WITH_SANITY_CHECKS             0
#define WITH_DEBUG_LOCKS               0

#define ENABLE_STATS                   1
#define ENABLE_MIGRATION_STATS         1
#define PRINT_PER_CORE_STATS           0

#define ENABLE_PINGPONG_FIX            0 /** Once a ping pong have been detected, undo replication **/
#define ENABLE_PINGPONG_AGGRESSIVE_FIX 0 /** Once a page has been written by two domains, undo replication **/
#define ENABLE_COLLAPSE_FIX            0 /** If a write is done on a replicated page, undo replication **/
#define ENABLE_COLLAPSE_FREQ_FIX       1 /** If a high frequency of write is detected on on a replicated page, undo replication **/

#define FAKE_REPLICATION               0 /** Use this if you just want to measure the cost of maintaining multiple mms **/
#define LAZY_REPLICATION               0 /** Do not copy the pte when copying the mm -- TODO**/

#if ENABLE_COLLAPSE_FREQ_FIX
#define MAX_NR_COLLAPSE_PER_PAGE       5
#endif

#if (ENABLE_PINGPONG_FIX + ENABLE_PINGPONG_AGGRESSIVE_FIX + ENABLE_COLLAPSE_FIX + ENABLE_COLLAPSE_FREQ_FIX) > 1
#error "You can only enable one of these options at a time"
#endif

#if (ENABLE_COLLAPSE_FREQ_FIX && ENABLE_PINGPONG_AGGRESSIVE_FIX && ! ENABLE_PERPAGE_STATS)
#error "Hmm issue: ENABLE_PERPAGE_STATS must be set to 1!"
#endif

#define PROCFS_REPLICATE_STATS_FN   "carrefour_replication_stats"

#endif
