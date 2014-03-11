#ifndef __REPLICATE_OPTIONS__
#define __REPLICATE_OPTIONS__

/** Configuration of replication internal stuff **/
// Look at include/linux/replicate.h for the headers

#define VERBOSE_PGFAULT                0
#define VERBOSE_REPTHREAD              0
#define VERBOSE_OTHERS                 0

#define WITH_SANITY_CHECKS             0
#define WITH_DEBUG_LOCKS               0

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

#if WITH_DEBUG_LOCKS
#ifndef CONFIG_DEBUG_SPINLOCK
#error "DEBUG_LOCKS requires CONFIG_DEBUG_SPINLOCK"
#endif

//op = 0 --> wait, op = 1 --> acquire, op = 2 --> release
#define __DEBUG_LOCKS(op, lock, fun, lock_wait, lock_owns, lock_fun) { \
   if(op == 0) {\
      lock_wait = lock; \
      lock_fun  = fun; \
   } \
   else if (op == 1) { \
      lock_wait = NULL; \
      lock_owns = lock; \
      lock_fun  = fun; \
   } \
   else { \
      lock_wait = NULL; \
      lock_owns = NULL; \
      lock_fun  = NULL; \
   } \
}

#define DEBUG_SEM_LOCKS(op, type) { \
   __DEBUG_LOCKS(op, sem, __builtin_return_address(0), current->rw_lock_sem_wait, current->rw_lock_sem_owns, current->rw_lock_fun); \
   current->rw_lock_type = type; \
}

#define DEBUG_SPIN_LOCKS(op) { \
   __DEBUG_LOCKS(op, lock, __builtin_return_address(0), current->spinlock_lock_wait, current->spinlock_lock_owns, current->spinlock_fun); \
   if(op == 2) { \
      lock->owner = NULL; \
   } \
   else if (op == 1) { \
      lock->owner = current; \
   } \
}

#else
#define DEBUG_SEM_LOCKS(op, type) do {} while(0)
#define DEBUG_SPIN_LOCKS(op) do {} while(0)

#endif //DEBUG_LOCKS

#endif
