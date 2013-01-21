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

#define ARRAY_NB_ENTRIES	(1024 * 8)

static int *array;
unsigned long * thread_to_core;
int ncores;

__attribute__((optimize("O0"))) void* read_a(void* thread_nr)
{
   unsigned long _core_nr = thread_to_core[(unsigned long) thread_nr];
   unsigned long _thread_nr = (unsigned long) thread_nr;

   set_affinity(gettid(), _thread_nr);
   printf("Starting...\n");
   for(int i=0; i<ARRAY_NB_ENTRIES; i++) {
      if(i == 300 && _thread_nr == 0)
         assert(madvise(array, ARRAY_NB_ENTRIES*sizeof(int), MADV_DONTREPLICATE) == 0);
      while(array[i] != _thread_nr){}
      printf("[Core %lu] Entry %d\n", _core_nr, i);
      array[i] = (array[i]+1)%ncores;

      while(array[i] != _thread_nr){}
      array[i] = (array[i]+1)%ncores;
   }

   printf("[CORE %lu]. Done. Have read %d entries\n", _core_nr, ARRAY_NB_ENTRIES);

   return NULL;
}

int main()
{
   ncores = get_nprocs();

   set_affinity(gettid(), 0);

   pthread_t threads[ncores];
   assert(posix_memalign((void**)&array, sysconf(_SC_PAGESIZE), ARRAY_NB_ENTRIES*sizeof(int)) == 0);

   for(int i=0; i<ARRAY_NB_ENTRIES; i++)
      array[i] = 0;

   /* Be sure to shuffle threads on nodes */
   thread_to_core = (unsigned long*) calloc(ncores, sizeof(unsigned long));

   assert(thread_to_core);
   shuffle_threads_on_nodes(thread_to_core, ncores);

   assert(madvise(array, ARRAY_NB_ENTRIES*sizeof(int), MADV_REPLICATE) == 0);

   for(unsigned long i=0; i<ncores; i++){
      pthread_create(&threads[i], NULL, read_a, (void*) i);
   }

   for(unsigned long i=0; i<ncores; i++){
      pthread_join(threads[i], NULL);
   }

   free(array);
   free(thread_to_core);
   return 0;

}
