#ifndef __COMMON__
#define __COMMON__

#ifdef LEGACY_MADV_REP
#define MADV_REPLICATE 	   16
#define MADV_DONTREPLICATE 17
#else
#define MADV_REPLICATE 	   63
#define MADV_DONTREPLICATE 64
#endif

pid_t gettid(void);
void set_affinity(unsigned long tid, unsigned long core_id);
unsigned long get_first_core_of_node (unsigned long node);
void shuffle_threads_on_nodes (unsigned long * thread_to_core, unsigned int nthreads);

#endif
