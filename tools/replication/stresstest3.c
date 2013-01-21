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
#define ARRAY_NB_ENTRIES	100

static int *array;

void* read_a(void* core_no)
{
   set_affinity(gettid(), (unsigned long) (core_no));

   for(int i=0; i<ARRAY_NB_ENTRIES; i++) {
      printf("[Core %d] %d\n", sched_getcpu(), array[i]);
   }
   return NULL;
}

int main()
{
   assert(numa_num_configured_nodes() >= 2);
   set_affinity(gettid(), get_first_core_of_node(0));

   assert(posix_memalign((void**)&array, sysconf(_SC_PAGESIZE), ARRAY_NB_ENTRIES*sizeof(int)) == 0);

   for(int i=0; i<ARRAY_NB_ENTRIES; i++)
      array[i] = i;

   assert(madvise(array, ARRAY_NB_ENTRIES*sizeof(int), MADV_REPLICATE) == 0);

   pthread_t new_thread;
   pthread_create(&new_thread, NULL, read_a, (void*)0UL);

   read_a((void*)1UL);

   pthread_join(new_thread, NULL);

   free(array);
   return 0;

}
