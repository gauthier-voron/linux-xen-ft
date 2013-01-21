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

#define ARRAY_NB_ENTRIES	(1024*1024*8) //64MB
#define NR_ITERATIONS      10

static int *array;

__attribute__((optimize("O0"))) void* read_a(void* core_nr)
{
   unsigned long _core_nr = (unsigned long) core_nr;
   set_affinity(gettid(), _core_nr);

   int fake_sum = 0;
   for(int i=0; i<ARRAY_NB_ENTRIES; i++) {
      //printf("[CORE %lu] %d\n", _core_nr, array[i]);
      fake_sum += array[i];
   }

   printf("[CORE %lu]. Done. Have read %d entries\n", _core_nr, ARRAY_NB_ENTRIES);

   return NULL;
}

int main()
{
   int ncores = get_nprocs();

   set_affinity(gettid(), 0);

   pthread_t threads[ncores];
   assert(posix_memalign((void**)&array, sysconf(_SC_PAGESIZE), ARRAY_NB_ENTRIES*sizeof(int)) == 0);

   for(int i=0; i<ARRAY_NB_ENTRIES; i++)
      array[i] = i;

   assert(madvise(array, ARRAY_NB_ENTRIES*sizeof(int), MADV_REPLICATE) == 0);

   /* Be sure to shuffle threads on nodes */
   unsigned long * thread_to_core = (unsigned long*) calloc(ncores, sizeof(unsigned long));
   shuffle_threads_on_nodes(thread_to_core, ncores);

   for(int ni = 0; ni < NR_ITERATIONS; ni++) {
      printf("-- Iteration %d\n", ni+1);
      for(int i=0; i<ARRAY_NB_ENTRIES; i++)
         array[i] = i;

      for(unsigned long i=1; i<ncores; i++){
         pthread_create(&threads[i], NULL, read_a, (void*)thread_to_core[i]);
      }

      read_a((void*)thread_to_core[0]);

      for(unsigned long i=1; i<ncores; i++){
         pthread_join(threads[i], NULL);
      }
   }

   free(array);
   free(thread_to_core);
   return 0;

}
