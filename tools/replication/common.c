#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <numa.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>

pid_t gettid(void) {
   return syscall(__NR_gettid);
}

void set_affinity(unsigned long tid, unsigned long core_id) {
   cpu_set_t mask;
   CPU_ZERO(&mask);
   CPU_SET(core_id, &mask);

   int r = sched_setaffinity(tid, sizeof(mask), &mask);
   if (r < 0) {
      fprintf(stderr, "couldn't set affinity for %lu\n", core_id);
      exit(1);
   }

   printf("Setting affinity of thread %lu on core %lu\n", tid, core_id);
}

unsigned long get_first_core_of_node (unsigned long node) {
   struct bitmask* bm;
   bm = numa_allocate_cpumask();
   numa_node_to_cpus(node, bm);

   for(int  i = 0; i < get_nprocs(); i++) {
      if (numa_bitmask_isbitset(bm, i)) {
         return i;
      }
   }

   printf("Oops\n");
   exit(EXIT_FAILURE);
}

void shuffle_threads_on_nodes (unsigned long * thread_to_core, unsigned int nthreads) {
   int ndies = numa_num_configured_nodes();
   int ncores = get_nprocs();
   struct bitmask ** bm = (struct bitmask**) malloc(sizeof(struct bitmask*) * ndies);

   for (int i = 0; i < ndies; i++) {
      bm[i] = numa_allocate_cpumask();
      numa_node_to_cpus(i, bm[i]);
   }

   int next_node = 0;
   for(int  i = 0; i < nthreads; i++) {
      int idx = (i / ndies) + 1;
      int found = 0;
      int j = 0;

      do {
         if (numa_bitmask_isbitset(bm[next_node], j)) {
            found++;
         }

         if(found == idx){
            //printf("Thread %d to core %d\n", i, j);
            thread_to_core[i] = j;
            break;
         }

         j = (j + 1) % ncores;
      } while (found != idx);

      next_node = (next_node + 1) % ndies;
   }

}
