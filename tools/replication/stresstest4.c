#define _GNU_SOURCE
#include <sys/mman.h>
#include <pthread.h>
#include <stdio.h>
#include <assert.h>
#include <numa.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>

#include "common.h"

#define ARRAY_NB_ENTRIES	1024
#define NR_JOIN         	10
#define NTHREADS		      2

static int *array;
unsigned long * thread_to_core;

__attribute__((optimize("O0"))) void* read_a(void* thread_nr)
{
   unsigned long _core_nr = thread_to_core[(unsigned long) thread_nr];
   unsigned long _thread_nr = (unsigned long) thread_nr;

   set_affinity(gettid(), _core_nr);

   for(int i=0; i<NR_JOIN; i++) {
      while(array[i] != _thread_nr){}
      printf("[Core %lu] Entry %d\n", _core_nr, i);
      array[i] = (array[i]+1)%NTHREADS;

      while(array[i] != _thread_nr){}
      array[i] = (array[i]+1)%NTHREADS;
   }

   printf("[CORE %lu]. Done. Have read %d entries\n", _core_nr, ARRAY_NB_ENTRIES);

   return NULL;
}

int main()
{
   set_affinity(gettid(), 0);

   pthread_t threads[NTHREADS];
   assert(posix_memalign((void**)&array, sysconf(_SC_PAGESIZE), ARRAY_NB_ENTRIES*sizeof(int)) == 0);

   for(int i=0; i<ARRAY_NB_ENTRIES; i++)
      array[i] = 0;

   /* Be sure to shuffle threads on nodes */
   thread_to_core = (unsigned long*) calloc(NTHREADS, sizeof(unsigned long));

   assert(thread_to_core);
   shuffle_threads_on_nodes(thread_to_core, NTHREADS);

   assert(madvise(array, ARRAY_NB_ENTRIES*sizeof(int), MADV_REPLICATE) == 0);

   for(unsigned long i=0; i<NTHREADS; i++){
      pthread_create(&threads[i], NULL, read_a, (void*) i);
   }

   for(unsigned long i=0; i<NTHREADS; i++){
      pthread_join(threads[i], NULL);
   }

   free(array);
   free(thread_to_core);
   return 0;

}
