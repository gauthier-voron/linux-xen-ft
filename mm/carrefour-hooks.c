#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/hugetlb.h>
#include <linux/migrate.h>
#include <linux/memcontrol.h>
#include <linux/mm_inline.h>
#include "internal.h"

#include <linux/carrefour-hooks.h>
#include <linux/replicate.h>

struct carrefour_options_t carrefour_default_options = {
   .page_bouncing_fix = 0,
};

struct carrefour_options_t carrefour_options;
struct carrefour_hook_stats_t carrefour_hook_stats;

int page_status_for_carrefour(int pid, unsigned long addr, int * alread_treated, int * huge) {
   struct task_struct *task;
   struct mm_struct *mm = NULL;
   struct vm_area_struct *vma;
   struct page * page;
   int ret = -1;
   int err;
   int flags = 0;

   *alread_treated = *huge = 0;

   rcu_read_lock();
   task = find_task_by_vpid(pid);
   if(task) {
      mm = get_task_mm(task);
   }
   rcu_read_unlock();
   if (!mm)
      return -1;

   down_read(&mm->mmap_sem);

   vma = find_vma(mm, addr);
   if (!vma || addr < vma->vm_start)
      goto out_locked;

   if (!is_vm_hugetlb_page(vma)) {
      flags |= FOLL_GET;
   }
 
   page = follow_page(vma, addr, flags);

   err = PTR_ERR(page);
   if (IS_ERR(page) || !page)
      goto out_locked;

   ret = 0;

   if(page->stats.nr_migrations) { // Page has been migrated already once
      *alread_treated = 1;
   }
   else if(PageReplication(page)) {
      *alread_treated = 1;
   }
 
   if (is_vm_hugetlb_page(vma)) {
      if(PageHuge(page)) {
         *huge = 1;
      }
      else {
         DEBUG_PANIC("[WARNING] How could it be possible ?\n");
      }
   }

   if(transparent_hugepage_enabled(vma) && (PageTransHuge(page))) {
      *huge = 2;
   }

   if(flags & FOLL_GET) {
      put_page(page);
   }

out_locked:
   //printk("[Core %d, PID %d] Releasing mm lock (0x%p)\n", smp_processor_id(), pid, &mm->mmap_sem);
   up_read(&mm->mmap_sem);
   mmput(mm);

   return ret;
}
EXPORT_SYMBOL(page_status_for_carrefour);

void reset_carrefour_stats (void) {
   memset(&carrefour_hook_stats, 0, sizeof(struct carrefour_hook_stats_t));
}
EXPORT_SYMBOL(reset_carrefour_stats);


void reset_carrefour_hooks (void) {
   carrefour_options = carrefour_default_options;
   reset_carrefour_stats();
}
EXPORT_SYMBOL(reset_carrefour_hooks);


void configure_carrefour(struct carrefour_options_t options) {
   carrefour_options = options;
}
EXPORT_SYMBOL(configure_carrefour);


struct carrefour_hook_stats_t* get_carrefour_hook_stats(void) {
   return &carrefour_hook_stats;
}
EXPORT_SYMBOL(get_carrefour_hook_stats);

static struct page *new_page_node(struct page *p, unsigned long on_node, int **result)
{
	return alloc_pages_exact_node(on_node, GFP_HIGHUSER_MOVABLE | GFP_THISNODE, 0);
}


int s_migrate_pages(pid_t pid, unsigned long nr_pages, void ** pages, int * nodes) {
   struct task_struct *task;
   struct mm_struct *mm = NULL;
   struct list_head * migratepages_nodes[MAX_NUMNODES];

   int i = 0;
   u64 start, end;


   rdtscll(start);

   for(i = 0; i < num_online_nodes(); i++) {
      migratepages_nodes[i] = kmalloc(sizeof (struct list_head), GFP_KERNEL);
      if(!migratepages_nodes[i]) {
         int j;
         for(j = 0; j < i; j++) {
            kfree(migratepages_nodes[j]);
         }

         DEBUG_WARNING("Cannot allocate memory !\n");
         return -ENOMEM;
      }
      INIT_LIST_HEAD(migratepages_nodes[i]);
   }

   rcu_read_lock();
   task = find_task_by_vpid(pid);
   if(task) {
      mm = get_task_mm(task);
   }
   rcu_read_unlock();

   if (!mm) {
      for(i = 0; i < num_online_nodes(); i++) {
         kfree(migratepages_nodes[i]);
      }

      return -EINVAL;
   }

   down_read(&mm->mmap_sem);

   for(i = 0; i < nr_pages; i++) {
      struct page * page;
      struct vm_area_struct *vma;

      unsigned long addr = (unsigned long) pages[i];
      int current_node;

      vma = find_vma(mm, addr);
      if (!vma || addr < vma->vm_start) {
         continue;
      }

      page = follow_page(vma, addr, FOLL_GET);

      if (IS_ERR(page) || !page) {
         continue;
      }

      /* Don't want to migrate a replicated page */
      if (PageReplication(page)) {
         goto next;
      }

      if(carrefour_options.page_bouncing_fix && (page->stats.nr_migrations >= carrefour_options.page_bouncing_fix)) {
         goto next; 
      }

      current_node = page_to_nid(page);

      /** Maybe we can do something similar to migrate.c to batch migrations **/
      if(current_node != nodes[i]) {
         if(!isolate_lru_page(page)) {
            list_add_tail(&page->lru, migratepages_nodes[nodes[i]]);
            inc_zone_page_state(page, NR_ISOLATED_ANON + page_is_file_cache(page));

            /** Not very precise because migration can fail **/
            carrefour_hook_stats.migr_from_to_node[current_node][nodes[i]]++;
            carrefour_hook_stats.real_nb_migrations++;
         }
         else {
            DEBUG_WARNING("[WARNING] Migration of page 0x%lx failed !\n", addr);
         }
      }
next:
      put_page(page);
   }

   for(i = 0; i < num_online_nodes(); i++) {
      if (!list_empty(migratepages_nodes[i])) {
         int err;
         err = migrate_pages(migratepages_nodes[i], new_page_node, i, 0, MIGRATE_SYNC, MR_NUMA_MISPLACED);
         if (err) {
            DEBUG_WARNING("[WARNING] Migration of pages on node %d failed !\n", i);
            putback_lru_pages(migratepages_nodes[i]);
         }
      }
      kfree(migratepages_nodes[i]);
	}

   up_read(&mm->mmap_sem);
   mmput(mm);

   rdtscll(end);
   carrefour_hook_stats.time_spent_in_migration = (end - start);

   return 0;
}

EXPORT_SYMBOL(s_migrate_pages);

// Quick and dirty for now. TODO: update
int move_thread_to_node(pid_t tid, int node) {
   int ret;
   ret = sched_setaffinity(tid, cpumask_of_node(node));
   sched_setaffinity(tid, cpu_online_mask);

   return ret;
}
EXPORT_SYMBOL(move_thread_to_node);

struct task_struct * get_task_struct_from_pid(int pid) {
   struct task_struct * task;

   rcu_read_lock();
   task = find_task_by_vpid(pid);
   if(task) {
      get_task_struct(task);
   }
   rcu_read_unlock();

   return task;
}
EXPORT_SYMBOL(get_task_struct_from_pid);

static int __init carrefour_hooks_init(void) {
   printk("Initializing Carrefour hooks\n");
   reset_carrefour_hooks();
   return 0;
}
module_init(carrefour_hooks_init)
