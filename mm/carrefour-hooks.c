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
   .use_balance_numa_api = 0,
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

   int use_balance_numa_api = carrefour_options.use_balance_numa_api;

   int i = 0;
   int err = 0;

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
      err = -ESRCH;
      goto out_clean;
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
         //DEBUG_WARNING("Cannot migrate a NULL page\n");
         continue;
      }

      /* Don't want to migrate a replicated page */
      if (PageReplication(page)) {
         put_page(page);
         continue;
      }

      if (PageHuge(page) || PageTransHuge(page)) {
         DEBUG_WARNING("[WARNING] What am I doing here ?\n");
         put_page(page);
         continue;
      }

      if(carrefour_options.page_bouncing_fix && (page->stats.nr_migrations >= carrefour_options.page_bouncing_fix)) {
         //DEBUG_WARNING("Page bouncing fix enable\n");
         put_page(page);
         continue;
      }

      current_node = page_to_nid(page);
      if(current_node == nodes[i]) {
         //DEBUG_WARNING("Current node (%d) = destination node (%d) for page 0x%lx\n", current_node, nodes[i], addr);
         put_page(page);
         continue;
      }

      if(use_balance_numa_api) {
         if(migrate_misplaced_page(page, nodes[i])) {
            //__DEBUG("Migrating page 0x%lx\n", addr);

            carrefour_hook_stats.migr_from_to_node[current_node][nodes[i]]++;
            carrefour_hook_stats.real_nb_migrations++;
         }
         else {
            //DEBUG_WARNING("[WARNING] Migration of page 0x%lx failed !\n", addr);
         }
      }
      else {
         /** Similar to migrate.c : Batching migrations **/
         if(!isolate_lru_page(page)) {
            //__DEBUG("Migrating page 0x%lx\n", addr);
            list_add_tail(&page->lru, migratepages_nodes[nodes[i]]);
            inc_zone_page_state(page, NR_ISOLATED_ANON + page_is_file_cache(page));

            /** Not very precise because migration can fail **/
            carrefour_hook_stats.migr_from_to_node[current_node][nodes[i]]++;
            carrefour_hook_stats.real_nb_migrations++;
         }
         else {
            //DEBUG_WARNING("[WARNING] Migration of page 0x%lx failed !\n", addr);
         }
         put_page(page);
      }
   }

   if(! use_balance_numa_api) {
      for(i = 0; i < num_online_nodes(); i++) {
         if (!list_empty(migratepages_nodes[i])) {
            err = migrate_pages(migratepages_nodes[i], new_page_node, i, MIGRATE_SYNC, MR_NUMA_MISPLACED);
            if (err) {
               DEBUG_WARNING("[WARNING] Migration of pages on node %d failed !\n", i);
               putback_lru_pages(migratepages_nodes[i]);
            }
         }
      }
   }

   up_read(&mm->mmap_sem);
   mmput(mm);

out_clean:
   if(use_balance_numa_api) {
      for(i = 0; i < num_online_nodes(); i++) {
         kfree(migratepages_nodes[i]);
      }
   }

   rdtscll(end);
   carrefour_hook_stats.time_spent_in_migration += (end - start);
   carrefour_hook_stats.s_migrate_nb_calls ++;

   return err;
}
EXPORT_SYMBOL(s_migrate_pages);

static struct page *new_page(struct page *p, unsigned long private, int **x)
{
   // private containe the destination node
   return alloc_huge_page_node(page_hstate(p), private);
}

int s_migrate_hugepages(pid_t pid, unsigned long nr_pages, void ** pages, int * nodes) {
   struct task_struct *task;
   struct mm_struct *mm = NULL;

   int i = 0;
   uint64_t start_migr, end_migr;
   rdtscll(start_migr);

   rcu_read_lock();
   task = find_task_by_vpid(pid);
   if(task) {
      mm = get_task_mm(task);
   }
   rcu_read_unlock();

   if (!mm)
      return -ESRCH;

	down_read(&mm->mmap_sem);

   for(i = 0; i < nr_pages; i++) {
      struct page * hpage;

      // Get the current page
      struct vm_area_struct *vma;
      int ret;

      unsigned long addr = (unsigned long) pages[i];
      int current_node;

      vma = find_vma(mm, addr);
      //if (!vma || pp->addr < vma->vm_start || !vma_migratable(vma))
      if (!vma || addr < vma->vm_start || !is_vm_hugetlb_page(vma))
         continue;

      hpage = follow_page(vma, addr, 0);

      if (IS_ERR(hpage) || !hpage)
         continue;

      if(hpage != compound_head(hpage)) {
         DEBUG_WARNING("[WARNING] What's going on ?\n");
         continue;
      }

      if(! PageHuge(hpage)) {
         DEBUG_WARNING("[WARNING] What am I doing here ?\n");
         continue;
      }

      current_node = page_to_nid(hpage);
      if(current_node != nodes[i]) {
         // Migrate the page
         if(get_page_unless_zero(hpage)) {
            ret = migrate_huge_page(hpage, new_page, nodes[i], MIGRATE_SYNC);
            put_page(hpage);

            if(ret) {
               printk("[WARNING] Migration of page 0x%lx failed !\n", addr);
            }
            else {
               carrefour_hook_stats.migr_from_to_node[current_node][nodes[i]]++;
               carrefour_hook_stats.real_nb_migrations++;
            }
         }
      }
   }

   up_read(&mm->mmap_sem);
   mmput(mm);

   rdtscll(end_migr);
   carrefour_hook_stats.time_spent_in_migration = (end_migr - start_migr);

   return 0;
}
EXPORT_SYMBOL(s_migrate_hugepages);

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

int find_and_split_thp(int pid, unsigned long addr) {
   struct task_struct *task;
   struct mm_struct *mm = NULL;
   struct vm_area_struct *vma;
   struct page * page;
   int ret = 1;
   int err;

   rcu_read_lock();
   task = find_task_by_vpid(pid);
   if(task) {
      mm = get_task_mm(task);
   }
   rcu_read_unlock();
   if (!mm) {
      ret = -ESRCH;
      return ret;
   }

   down_read(&mm->mmap_sem);

   vma = find_vma(mm, addr);
   if (!vma || addr < vma->vm_start) {
      ret = -EPAGENOTFOUND;
      goto out_locked;
   }

   if (!transparent_hugepage_enabled(vma)) {
      ret = -EINVALIDPAGE;
      goto out_locked;
   }
 
   page = follow_page(vma, addr, FOLL_GET);

   err = PTR_ERR(page);
   if (IS_ERR(page) || !page) {
      ret = -EPAGENOTFOUND;
      goto out_locked;
   }

   if(PageTransHuge(page)) {
      // We found the page. Split it.
      // split_huge_page does not create new pages. It will simple create new ptes and update the pmd
      // see  __split_huge_page_map for details
      ret = split_huge_page(page);
   }
   else {
      ret = -EINVALIDPAGE;
   }

   put_page(page);

out_locked:
   //printk("[Core %d, PID %d] Releasing mm lock (0x%p)\n", smp_processor_id(), pid, &mm->mmap_sem);
   up_read(&mm->mmap_sem);
   mmput(mm);

   return ret;
}
EXPORT_SYMBOL(find_and_split_thp);

int find_and_migrate_thp(int pid, unsigned long addr, int to_node) {
   struct task_struct *task;
   struct mm_struct *mm = NULL;
   struct vm_area_struct *vma;
   struct page *page;
   int ret = -1;
   int current_node;

   pgd_t *pgd;
   pud_t *pud;
   pmd_t *pmd, orig_pmd;

   rcu_read_lock();
   task = find_task_by_vpid(pid);
   if(task) {
      mm = get_task_mm(task);
   }
   rcu_read_unlock();
   if (!mm) {
      ret = -ESRCH;
      return ret;
   }

   down_read(&mm->mmap_sem);

   vma = find_vma(mm, addr);
   if (!vma || addr < vma->vm_start) {
      ret = -EPAGENOTFOUND;
      goto out_locked;
   }

   if (!transparent_hugepage_enabled(vma)) {
      ret = -EINVALIDPAGE;
      goto out_locked;
   }
 
   pgd = pgd_offset(mm, addr);
   if (!pgd_present(*pgd )) {
      ret = -EINVALIDPAGE;
      goto out_locked;
   }

   pud = pud_offset(pgd, addr);
   if(!pud_present(*pud)) {
      ret = -EINVALIDPAGE;
      goto out_locked;
   }

   pmd = pmd_offset(pud, addr);
   if (!pmd_present(*pmd ) || !pmd_trans_huge(*pmd)) {
      ret = -EINVALIDPAGE;
      goto out_locked;
   }

   // We found a valid pmd for this address and that's a THP
   orig_pmd = *pmd;

   // Mostly copied from the do_huge_pmd_numa_page function
	spin_lock(&mm->page_table_lock);
	if (unlikely(!pmd_same(orig_pmd, *pmd))) {
      spin_unlock(&mm->page_table_lock);
		goto out_locked;
   }

	page = pmd_page(*pmd);
	get_page(page);

	current_node = page_to_nid(page);

   if(current_node == to_node) {
		put_page(page);
      spin_unlock(&mm->page_table_lock);
		goto out_locked;
   }
	spin_unlock(&mm->page_table_lock);

	/* Acquire the page lock to serialise THP migrations */
	lock_page(page);

	/* Confirm the PTE did not while locked */
	spin_lock(&mm->page_table_lock);
	if (unlikely(!pmd_same(orig_pmd, *pmd))) {
		unlock_page(page);
		put_page(page);
      spin_unlock(&mm->page_table_lock);
		goto out_locked;
	}

   // Make sure that pmd is tagged as "NUMA"
   orig_pmd = pmd_mknuma(orig_pmd);
   set_pmd_at(mm, addr & PMD_MASK, pmd, orig_pmd);
   
	spin_unlock(&mm->page_table_lock);

	/* Migrate the THP to the requested node */
	ret = migrate_misplaced_transhuge_page(mm, vma, pmd, orig_pmd, addr, page, to_node);

	if (ret > 0) {
      //__DEBUG("Migrated THP 0x%lx successfully\n", addr);
      carrefour_hook_stats.migr_from_to_node[current_node][to_node]++;
      carrefour_hook_stats.real_nb_migrations++;
      ret = 0;
   }
   else {
      //__DEBUG("Failed migrating THP 0x%lx (ret = %d)\n", addr, ret);
      ret = -1;
      // put page has been performed by migrate_misplaced_transhuge_page
      
      // It failed
      // We need to clear the pmd_numa_flag ...   
      spin_lock(&mm->page_table_lock);
      if (pmd_same(orig_pmd, *pmd)) {
         orig_pmd = pmd_mknonnuma(orig_pmd);
         set_pmd_at(mm, addr, pmd, orig_pmd);
         VM_BUG_ON(pmd_numa(*pmd));
         update_mmu_cache_pmd(vma, addr, pmd);
      }
      spin_unlock(&mm->page_table_lock);
	}

out_locked:
   //printk("[Core %d, PID %d] Releasing mm lock (0x%p)\n", smp_processor_id(), pid, &mm->mmap_sem);
   up_read(&mm->mmap_sem);
   mmput(mm);

   return ret;
}
EXPORT_SYMBOL(find_and_migrate_thp);


static int __init carrefour_hooks_init(void) {
   printk("Initializing Carrefour hooks\n");
   reset_carrefour_hooks();
   return 0;
}
module_init(carrefour_hooks_init)
