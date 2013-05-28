/* JRF */
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/unistd.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/completion.h>
#include <linux/personality.h>
#include <linux/mempolicy.h>
#include <linux/sem.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/iocontext.h>
#include <linux/key.h>
#include <linux/binfmts.h>
#include <linux/mman.h>
#include <linux/mmu_notifier.h>
#include <linux/fs.h>
#include <linux/nsproxy.h>
#include <linux/capability.h>
#include <linux/cpu.h>
#include <linux/cgroup.h>
#include <linux/security.h>
#include <linux/hugetlb.h>
#include <linux/swap.h>
#include <linux/syscalls.h>
#include <linux/jiffies.h>
#include <linux/tracehook.h>
#include <linux/futex.h>
#include <linux/compat.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/rcupdate.h>
#include <linux/ptrace.h>
#include <linux/mount.h>
#include <linux/audit.h>
#include <linux/memcontrol.h>
#include <linux/ftrace.h>
#include <linux/profile.h>
#include <linux/rmap.h>
#include <linux/ksm.h>
#include <linux/acct.h>
#include <linux/tsacct_kern.h>
#include <linux/cn_proc.h>
#include <linux/freezer.h>
#include <linux/delayacct.h>
#include <linux/taskstats_kern.h>
#include <linux/random.h>
#include <linux/tty.h>
#include <linux/proc_fs.h>
#include <linux/blkdev.h>
#include <linux/fs_struct.h>
#include <linux/magic.h>
#include <linux/perf_event.h>
#include <linux/posix-timers.h>
#include <linux/user-return-notifier.h>


#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/sched.h>
#include <linux/rwsem.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/spinlock.h>
#include <linux/jhash.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/memory.h>
#include <linux/mmu_notifier.h>
#include <linux/swap.h>
#include <linux/ksm.h>
#include <linux/hash.h>
#include <linux/freezer.h>
#include <linux/oom.h>
#include <linux/list.h>
#include <linux/mmzone.h>
#include <asm/mmzone_64.h>
#include <linux/numa.h>
#include <linux/replicate.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/mmu_context.h>
#include <asm/cacheflush.h>

#include <asm/tlbflush.h>
#include "internal.h"

#include <asm/tlb.h>

#define allocate_work_list_item() (kmem_cache_alloc(work_cachep, GFP_KERNEL))
#define free_work_list_item(item) (kmem_cache_free(work_cachep, (item)))

/** Data structures used later in this file **/
struct walk_infos {
   struct vm_area_struct *vma;
};

struct dup_infos {
   struct mm_struct * mm_src;
   struct mm_struct * mm_dest;
   int ret;
};

struct work_list_item {
	struct list_head list;
   struct mm_struct * mm;
   unsigned long start;
   unsigned long len;
   int behavior;
};

static LIST_HEAD(work_list);
static DEFINE_SPINLOCK(work_list_lock);
static struct task_struct *work_task;

struct task_struct * work_thread;
/** END **/

/** Internal variables **/
static struct kmem_cache *work_cachep;

#if ENABLE_STATS
DEFINE_RWLOCK(reset_stats_rwl);
DEFINE_PER_CPU(replication_stats_t, replication_stats_per_core);
#endif
/** END **/

/** Function headers **/
static int dup_page_table(struct mm_struct *mm, pgd_t *src_pgd, pgd_t *dest_pgd);
/** END **/

int replicate_madvise(pid_t tgid, unsigned long start, unsigned long len, int advice)
{
   struct work_list_item *new_work;
   struct task_struct *tsk;

   if(!work_thread) {
      DEBUG_REPTHREAD("repd is not running. Ignoring.\n");
      return EREPD_NOT_RUNNING;
   }

   new_work = allocate_work_list_item();
   if(!new_work) {
      DEBUG_PANIC("Cannot allocate a new_work\n");
   }

   rcu_read_lock();
   tsk = find_task_by_vpid(tgid);

   if(unlikely(!tsk || !tsk->mm)) {
      DEBUG_REPTHREAD("What am I suppose to do? (tgid = %d, tsk = %p, tsk->mm = %p)\n", tgid, tsk, tsk ? tsk->mm : NULL);

      free_work_list_item(new_work);

      rcu_read_unlock();
      return EPID_NOTFOUND;
   }

   new_work->start = start;
   new_work->mm = tsk->mm;
   new_work->len = len;
   new_work->behavior = advice;

   /* Always increase ref count, work thread will decrement
    * upon item completion
    */
   atomic_inc(&tsk->mm->mm_count);

   rcu_read_unlock();

   spin_lock(&work_list_lock);
   list_add(&new_work->list, &work_list);

   //DEBUG_REPTHREAD("Pushed a new work item in the queue. Tgid = %d, start = 0x%lx, len = 0x%lx\n", tgid, new_work->start, new_work->len);

   wake_up_process(work_task);
   spin_unlock(&work_list_lock);

	return 0;
}

EXPORT_SYMBOL(replicate_madvise);

/* mmap_sem should be held in read mode before calling this function */
int rep_copy_pgd_pte(struct mm_struct* mm, struct vm_area_struct * vma, pgd_t * src, pgd_t *dest, unsigned long address)
{
	pgd_t *mm_dest_pgd = rep_pgd_offset(dest, address);
	pud_t *mm_dest_pud;
	pmd_t *mm_dest_pmd;
	pte_t *mm_dest_pte, *mm_src_pte;

   spinlock_t *ptl;

	mm_dest_pud = pud_alloc(mm, mm_dest_pgd, address);
	if(!mm_dest_pud)
		return VM_FAULT_OOM;
	mm_dest_pmd = pmd_alloc(mm, mm_dest_pud, address);
	if(!mm_dest_pmd)
		return VM_FAULT_OOM;
	mm_dest_pte = pte_alloc_map(mm, vma, mm_dest_pmd, address);
	if(!mm_dest_pte)
		return VM_FAULT_OOM;

   mm_src_pte = get_locked_pte_from_va (src, mm, address, &ptl);
   if(!mm_src_pte) {
      // mm pte is not present. It could happen if the page is migrated / swapped for example
      // DEBUG_REPTHREAD("Cannot copy a non-existent pte... (address = 0x%lx)\n", page_va(address));
      goto out;
   }

   if(unlikely(pte_present(*mm_dest_pte))) {
      //DEBUG_REPTHREAD("Dest pte has already been set. Ignoring\n");
		goto out_unlock;
	}

   //ptep_clear_flush(vma, address, mm_dest_pte);
   set_pte_at_notify(mm, address, mm_dest_pte, *mm_src_pte);

   pte_unmap_unlock(mm_src_pte, ptl);
   return VM_FAULT_NOPAGE;

out_unlock:
   pte_unmap_unlock(mm_src_pte, ptl);
out:
   return VM_FAULT_RETRY;
}

static inline void rep_pgd_list_add(pgd_t *pgd) {
	struct page *page = virt_to_page(pgd);

	list_add(&page->lru, &pgd_list);
}

static void rep_pgd_set_mm(pgd_t *pgd, struct mm_struct *mm)
{
	BUILD_BUG_ON(sizeof(virt_to_page(pgd)->index) < sizeof(mm));
	virt_to_page(pgd)->index = (pgoff_t)mm;
}

static void rep_pgd_ctor(struct mm_struct *mm, pgd_t *pgd)
{
	/* If the pgd points to a shared pagetable level (either the
	   ptes in non-PAE, or shared PMD in PAE), then just copy the
	   references from swapper_pg_dir. */
	if (PAGETABLE_LEVELS == 2 ||
	    (PAGETABLE_LEVELS == 3 && SHARED_KERNEL_PMD) ||
	    PAGETABLE_LEVELS == 4) {
      clone_pgd_range(pgd + KERNEL_PGD_BOUNDARY,
				swapper_pg_dir + KERNEL_PGD_BOUNDARY,
				KERNEL_PGD_PTRS);
	}

	/* list required to sync kernel mapping updates */
	if (!SHARED_KERNEL_PMD) {
		rep_pgd_set_mm(pgd, mm);
		rep_pgd_list_add(pgd);
	}
}

/** Similar to pgd_alloc except that we don't pre-populate **/
static pgd_t * rep_pgd_alloc(struct mm_struct *mm)
{
	pgd_t *pgd;
	//pmd_t *pmds[PREALLOCATED_PMDS];

	pgd = (pgd_t *)__get_free_page(GFP_KERNEL|__GFP_NOTRACK|__GFP_REPEAT|__GFP_ZERO);

	if (pgd == NULL)
		goto out;

	spin_lock(&pgd_lock);
	rep_pgd_ctor(mm, pgd);
	spin_unlock(&pgd_lock);

	return pgd;

out:
	return NULL;
}

static int create_replicated_pgds(struct mm_struct * mm) {
   int node;

   if(is_replicated(mm)) {
      return 0;
   }

   /** We need the write lock on the mm to ensure no insertion/removal whilst duplicating the page table **/
   up_read(&mm->mmap_sem);
   down_write(&mm->mmap_sem);

   DEBUG_REPTHREAD("New replicated mm: duplicating pgd\n");
   for_each_online_node(node)
   {
      int err;
      mm->pgd_node[node] = rep_pgd_alloc(mm);
      if(mm->pgd_node[node] == NULL) {
         DEBUG_PANIC("pgd allocation failed\n");
      }

      DEBUG_REPTHREAD("New pgd for node %d : %p (master = %p)\n", node, mm->pgd_node[node], mm->pgd_master);
      err = dup_page_table(mm, mm->pgd_master, mm->pgd_node[node]);
   }
   DEBUG_REPTHREAD("MM lock %p\n", &mm->mmap_sem);

   atomic_inc(&mm->mm_count);

   /** Everything is consistent, we can set the mm as replicated **/
   mm->replicated_mm = 1;

   /** And release the write lock **/
   up_write(&mm->mmap_sem);
   down_read(&mm->mmap_sem);

   return 1;
}

static inline struct page* rep_find_page(struct mm_struct *mm, struct vm_area_struct * vma, unsigned long address, pte_t * pte) {
   struct page *page;

   page = vm_normal_page(vma, address, *pte);
   if (page) {
      if(PageAnon(page)) {
         if(PageReplication(page)) {
            DEBUG_REPTHREAD("Tried to replicate an already replicated page, ignoring\n");
         }
         else if (PagePingPong(page)) {
            DEBUG_REPTHREAD("Tried to replicate an old replicated page, ignoring\n");
         }
         else {
            if(!pte_write(*pte)) {
               DEBUG_REPTHREAD("Tried to replicate a write-protected page, ignoring\n");
            }
            else {
               goto out;
            }
         }
      }
      else {
         DEBUG_REPTHREAD("Tried to replicate a not anonymous page, ignoring\n");
      }
   }
   else {
      DEBUG_REPTHREAD("No \"normal\" page associated with address 0x%lx, ignoring\n", address);
   }
   return NULL;

out:
   return page;
}

/* must hold a reference and have the page locked when calling,
 * function will drop the reference and lock
 */
static inline int do_page_replication(struct mm_struct * mm, struct vm_area_struct * vma, unsigned long address, pte_t pte_entry, struct page * page)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
   spinlock_t *ptl;
   pte_t new_pte, *pte_master = NULL, *pte_slave;
   int node;
   struct page * allocated_pages[MAX_NUMNODES];

   DEBUG_REPTHREAD("Replicating page 0x%lx\n", page_va(address));

   /** First of all, we need to make sure that allocate a pte for each node pgd
       pud alloc / pmd_alloc / __pte_alloc are taking the page_table_lock
   **/
   for_each_online_node(node) {
      struct page * new_page;

      pgd = mm->pgd_node[node];
      pgd = rep_pgd_offset(pgd, address);

      pud = pud_alloc(mm, pgd, address);
      if(!pud)
         goto oom;

      pmd = pmd_alloc(mm, pud, address);
      if(!pmd)
         goto oom;

      if(__pte_alloc(mm, vma, pmd, address))
         goto oom;

      /* Allocate a new new_page on the node */
      new_page = alloc_page_interleave(GFP_HIGHUSER_MOVABLE, 0, node);
      if(!new_page) {
         goto oom;
      }
      __SetPageUptodate(new_page);

      allocated_pages[node] = new_page;
   }

   pte_master = get_locked_pte_from_va (mm->pgd_master, mm, address, &ptl);
   if(!pte_master) {
      goto fail;
   }

   if(unlikely(!pte_same(pte_entry, *pte_master))) {
      /* pte has changed -- Lets ignore it for now (because the replication decisions may change !) */
      pte_unmap_unlock(pte_master, ptl);
      goto fail;
   }

   /* Here we have the page. Updating its attributes */
   SetPageReplication(page);
   SetPageCollapsed(page);
   ClearPagePingPong(page);
   memset(&page->stats, 0, sizeof(perpage_stats_t));


   /* read/write protect pages in master */
   new_pte = mk_pte(page, PAGE_NONE);
   set_pte_at_notify(mm, address, pte_master, new_pte);
   flush_tlb_page(vma, address);

   /* Allocate a page for each domain, data will be copied lazily */
   for_each_online_node(node) {
      struct page *new_page = allocated_pages[node];

      pgd = rep_pgd_offset(mm->pgd_node[node], address);
      pud = pud_offset(pgd, address);
      pmd = pmd_offset(pud, address);
      pte_slave = pte_offset_map(pmd, address);

      /* Clear and flush entry if needed */
      if(pte_present(*pte_slave)) {
         ptep_clear_flush(vma, address, pte_slave);
      }

      /* Here we have the page. Updating its attributes */
      SetPageReplication(new_page);
      ClearPageCollapsed(new_page);
      ClearPagePingPong(new_page);
      memset(&new_page->stats, 0, sizeof(perpage_stats_t));

      /* No permissions because we aren't copying data now */
      new_pte = mk_pte(new_page, PAGE_NONE);

      /* Set up the new page */
      page_add_new_anon_rmap(new_page, vma, address);

      //DEBUG_REPTHREAD("Allocated a new page (0X%lx) for address 0x%lx on node %d\n", (unsigned long) page_address(new_page), page_va(address), node);

      /* Install the new mapping */
      set_pte_at_notify(mm, address, pte_slave, new_pte);

#if WITH_SANITY_CHECKS
      pte_slave = get_pte_from_va(mm->pgd_node[node], address);
      if(!pte_slave) {
         DEBUG_PANIC("Insertion has failed !\n");
      }
#endif
   }

   /* Release the lock on the master pte */
   pte_unmap_unlock(pte_master, ptl);
   return 0;

fail:
   for_each_online_node(node) {
      struct page * new_page = allocated_pages[node];

      /* Release it */
      page_cache_release(new_page);
   }
   return 1;
oom:
   DEBUG_PANIC("OOM error\n");
}

static int rep_update_pages(struct mm_struct * mm, unsigned long start, unsigned long end, unsigned long behavior)
{
   struct vm_area_struct *vma;
   struct page *page;
   unsigned long cur_address;
   pte_t pte_entry, *pte;
   spinlock_t *ptl;

   int ret = 0;

   for(cur_address = start; cur_address < end; cur_address += PAGE_SIZE) {
      if(! is_user_addr(cur_address)) {
         DEBUG_WARNING("Tried to replicate a kernel page, ignoring\n");
         INCR_REP_STAT_VALUE(nr_ignored_orders, 1);
         continue;
      }

      vma = find_vma(mm, cur_address);
      if (!vma || vma->vm_start > cur_address) {
         DEBUG_REPTHREAD("Cannot find the VMA for address %lx, ignoring\n", cur_address);
         INCR_REP_STAT_VALUE(nr_ignored_orders, 1);
         continue;
      }

      if (vma->vm_file || (vma->vm_flags & (VM_SHARED|VM_HUGETLB)) || !(vma->vm_flags & VM_READ) || !(vma->vm_flags & VM_WRITE)
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
         || (vma->vm_flags & VM_HUGEPAGE)
#endif
         ) {
         DEBUG_REPTHREAD("Cannot replicate a page (0x%lx) that belongs to a not-supported VMA, ignoring\n", cur_address);
         INCR_REP_STAT_VALUE(nr_ignored_orders, 1);
         continue;
      }

      pte = get_locked_pte_from_va (mm->pgd_master, mm, cur_address, &ptl);
      if(!pte) {
         DEBUG_REPTHREAD("Tried to replicate a not present pte (address = 0x%lx), ignoring\n", cur_address);
         INCR_REP_STAT_VALUE(nr_ignored_orders, 1);
         continue;
      }

      page = rep_find_page(mm, vma, cur_address, pte);
      pte_entry = *pte;
      pte_unmap_unlock(pte, ptl);

      if(page) {
         if(!do_page_replication(mm, vma, cur_address, pte_entry, page)) {
            ret = 1;
            INCR_REP_STAT_VALUE(nr_replicated_pages, 1);
         }
         else {
            INCR_REP_STAT_VALUE(nr_ignored_orders, 1);
         }
      }
      else {
         INCR_REP_STAT_VALUE(nr_ignored_orders, 1);
      }
   }

   return ret;
}

/** Todo: the TLB is probably flushed to many times **/
void clear_flush_all_node_copies (struct mm_struct * mm, struct vm_area_struct * vma, unsigned long address) {
   int flush_needed = 0;

   if(is_replicated(mm)) {
      int cur_node;
      DEBUG_REP_VV("Clearing and flushing all nodes for address 0x%lx (caller = %p)\n", address, __builtin_return_address(0));
      for_each_online_node(cur_node) {
         pte_t * pte = get_pte_from_va(mm->pgd_node[cur_node], address);
         if(pte) {
            struct page *page = pte_page(*pte);

            /* Clear pte */
            ptep_get_and_clear(mm, address, pte);

            /* Delay the flush */
            flush_needed = 1;

            if(page && PageReplication(page)) {
               DEBUG_REP_VV("This is a replicated page. Removing all copies\n");

               page_remove_rmap(page);
               if (unlikely(page_mapcount(page) < 0)) {
                  DEBUG_PANIC("That should not be possible !\n");
               }
            }
         }
      }
   }

   if(flush_needed) {
      flush_tlb_page(vma, address);
   }
}

static int rep_work_thread(void *nothing)
{
   struct list_head *pos, *q;
   struct work_list_item *work;
   unsigned long end;
   int last_online_cpu = 0, cpu;
   struct cpumask dstp;

   work_task = current;

   /** To ease the debug, we want to make sure that this thread is pinned on the last available CPU **/
   cpumask_clear(&dstp);

   for_each_online_cpu(cpu) {
      last_online_cpu = cpu;
   }
   cpumask_set_cpu(last_online_cpu, &dstp);
   sched_setaffinity(0, &dstp);

   DEBUG_REPTHREAD("Assigning repd to core %d\n", last_online_cpu);

   while(1)
   {
      spin_lock(&work_list_lock);
      if(list_empty(&work_list)) {
         set_current_state(TASK_INTERRUPTIBLE);
         spin_unlock(&work_list_lock);
         schedule();
         spin_lock(&work_list_lock);
      }

      list_for_each_safe(pos, q, &work_list) {
         work = list_entry(pos, struct work_list_item, list);
         spin_unlock(&work_list_lock);

         if(!work_thread) {
            goto exit;
         }

         work->start &= PAGE_MASK;
         /* end calculation is from madvise */
         end = work->start + ((work->len + ~PAGE_MASK) & PAGE_MASK);

         if(!work->mm) {
            continue;
         }

         if(work->behavior == MADV_REPLICATE) {
            int has_created_pgds;
            int has_replicated_page;

            DEBUG_REPTHREAD("New message received (start = %lu work->start, end = %lu)\n", work->start, end);

#if WITH_DEBUG_LOCKS
            DEBUG_PRINT("Acquiring writer lock %p (caller %p)\n", &work->mm->mmap_sem, rep_work_thread);
#endif
            down_read(&work->mm->mmap_sem);
#if WITH_DEBUG_LOCKS
            DEBUG_PRINT("Acquired writer lock %p (caller %p)\n", &work->mm->mmap_sem, rep_work_thread);
#endif

            if(!work_thread) {
               goto exit;
            }

            /* find_or_insert_group will grab the group sem in write mode */
            has_created_pgds = create_replicated_pgds(work->mm);

#if ! FAKE_REPLICATION
            /* set replication flag on pages and remove from slaves */
            has_replicated_page = rep_update_pages(work->mm, work->start, end, MADV_REPLICATE);
#endif

            DEBUG_REPTHREAD("Message processed properly (start = %lu work->start, end = %lu)\n", work->start, end);

#if 0 && WITH_SANITY_CHECKS
            if(has_created_pgds || has_replicated_page) {
               check_pgd_consistency(work->mm);
            }
#endif

            up_read(&work->mm->mmap_sem);
#if WITH_DEBUG_LOCKS
            DEBUG_PRINT("Released writer lock %p (caller %p)\n", &work->mm->mmap_sem, rep_work_thread);
#endif
         }
         else if(work->behavior == MADV_DONTREPLICATE) {
            DEBUG_PANIC("Not implemented yet.\n");
         }
         else {
            DEBUG_PANIC("work_list_item has incorrect flags\n");
         }


         atomic_dec(&work->mm->mm_count);
         spin_lock(&work_list_lock);
         list_del(pos);
         free_work_list_item(work);
      }

      spin_unlock(&work_list_lock);
   }

exit:
   return 0;
}

#if ENABLE_STATS
/**
PROCFS Functions
We create here entries in the proc system that will allows us to configure replication and gather stats :)
That's not very clean, we should use sysfs instead [TODO]
**/
static int display_replication_stats(struct seq_file *m, void* v)
{
   replication_stats_t* global_stats;
   int cpu, i, j;
   unsigned long time_rd_lock  = 0;
   unsigned long time_wr_lock  = 0;
   unsigned long time_lock     = 0;
   unsigned long time_pgfault  = 0;
   unsigned long nr_migrations = 0;

   seq_printf(m, "#Number of online cpus: %d\n", num_online_cpus());
   seq_printf(m, "#Number of online nodes: %d\n", num_online_nodes());

   /** Merging stats **/
   global_stats = kmalloc(sizeof(replication_stats_t), GFP_KERNEL);
   if(!global_stats) {
      DEBUG_PANIC("No more memory ?\n");
   }
   memset(global_stats, 0, sizeof(replication_stats_t));

   write_lock(&reset_stats_rwl);

   for_each_online_cpu(cpu) {
      replication_stats_t * stats = per_cpu_ptr(&replication_stats_per_core, cpu);

      // Automatic merging of everything
      for(i = 0; i < sizeof(replication_stats_t) / sizeof(uint64_t); i++) {
         if(((uint64_t*) stats) + i == &stats->max_nr_migrations_per_4k_page) {
            // We don't want to automerge this one
            continue;
         }
         ((uint64_t *) global_stats)[i] += ((uint64_t*) stats)[i];
      }

      if(stats->max_nr_migrations_per_4k_page > global_stats->max_nr_migrations_per_4k_page) {
         global_stats->max_nr_migrations_per_4k_page = stats->max_nr_migrations_per_4k_page;
      }
   }

   time_rd_lock = 0;
   time_wr_lock = 0;
   time_lock    = 0;
   time_pgfault = 0;

   if(global_stats->nr_readlock_taken) {
      time_rd_lock = (unsigned long) (global_stats->time_spent_acquiring_readlocks / global_stats->nr_readlock_taken);
   }
   if(global_stats->nr_writelock_taken) {
      time_wr_lock = (unsigned long) (global_stats->time_spent_acquiring_writelocks / global_stats->nr_writelock_taken);
   }
   if(global_stats->nr_readlock_taken + global_stats->nr_writelock_taken) {
      time_lock = (unsigned long) ((global_stats->time_spent_acquiring_readlocks + global_stats->time_spent_acquiring_writelocks) / (global_stats->nr_readlock_taken + global_stats->nr_writelock_taken));
   }
   if(global_stats->nr_pgfault) {
      time_pgfault = (unsigned long) (global_stats->time_spent_in_pgfault_handler / global_stats->nr_pgfault);
   }

   seq_printf(m, "[GLOBAL] Number of MM switch: %lu\n", (unsigned long) global_stats->nr_mm_switch);
   seq_printf(m, "[GLOBAL] Number of collapses: %lu\n", (unsigned long) global_stats->nr_collapses);
   seq_printf(m, "[GLOBAL] Number of ping pongs: %lu\n", (unsigned long) global_stats->nr_pingpong);
   seq_printf(m, "[GLOBAL] Number of reverted replication decisions: %lu\n", (unsigned long) global_stats->nr_replicated_decisions_reverted);
   seq_printf(m, "[GLOBAL] Number of replicated pages: %lu\n", (unsigned long) global_stats->nr_replicated_pages);
   seq_printf(m, "[GLOBAL] Number of ignored orders: %lu\n\n", (unsigned long) global_stats->nr_ignored_orders);

   seq_printf(m, "[GLOBAL] Time spent acquiring read locks: %lu cycles\n", time_rd_lock);
   seq_printf(m, "[GLOBAL] Time spent acquiring write locks: %lu cycles\n", time_wr_lock);
   seq_printf(m, "[GLOBAL] Time spent acquiring locks (global): %lu cycles\n\n", time_lock);

   seq_printf(m, "[GLOBAL] Number of page faults: %lu\n", (unsigned long) global_stats->nr_pgfault);
   seq_printf(m, "[GLOBAL] Time spent in the page fault handler: %lu cycles\n\n", time_pgfault);

   seq_printf(m, "[GLOBAL] 4k pages:\n");
   seq_printf(m, "[GLOBAL] Number of pages freed (i.e, approx. total number of pages): %lu\n", (unsigned long) global_stats->nr_4k_pages_freed);
   seq_printf(m, "[GLOBAL] Number of pages migrated at least once: %lu\n", (unsigned long) global_stats->nr_4k_pages_migrated_at_least_once);
   seq_printf(m, "[GLOBAL] Max number of migrations per page: %lu\n", (unsigned long) global_stats->max_nr_migrations_per_4k_page);

   for(i = 0; i < num_online_nodes(); i++) {
      seq_printf(m, "[GLOBAL] Moved pages from node %d: ", i);
      for(j = 0; j < num_online_nodes(); j++) {
         seq_printf(m,"%lu\t", (unsigned long) global_stats->migr_4k_from_to_node[i][j]);

         nr_migrations += global_stats->migr_4k_from_to_node[i][j];
      }
      seq_printf(m, "\n");
   }
   seq_printf(m, "[GLOBAL] Number of migrations: %lu\n\n", nr_migrations);

   seq_printf(m, "[GLOBAL] 2M pages:\n");
   seq_printf(m, "[GLOBAL] Number of pages freed (i.e, approx. total number of pages): %lu\n", (unsigned long) global_stats->nr_2M_pages_freed);
   seq_printf(m, "[GLOBAL] Number of pages migrated at least once: %lu\n", (unsigned long) global_stats->nr_2M_pages_migrated_at_least_once);
   seq_printf(m, "[GLOBAL] Max number of migrations per page: %lu\n", (unsigned long) global_stats->max_nr_migrations_per_2M_page);

   nr_migrations = 0;
   for(i = 0; i < num_online_nodes(); i++) {
      seq_printf(m, "[GLOBAL] Moved pages from node %d: ", i);
      for(j = 0; j < num_online_nodes(); j++) {
         seq_printf(m,"%lu\t", (unsigned long) global_stats->migr_2M_from_to_node[i][j]);

         nr_migrations += global_stats->migr_2M_from_to_node[i][j];
      }
      seq_printf(m, "\n");
   }
   seq_printf(m, "[GLOBAL] Number of migrations: %lu\n", nr_migrations);

   kfree(global_stats);

   write_unlock(&reset_stats_rwl);
   return 0;
}

static int replication_stats_open(struct inode *inode, struct file *file) {
   return single_open(file, display_replication_stats, NULL);
}

static ssize_t ibs_proc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
   int cpu;

   write_lock(&reset_stats_rwl);
   for_each_online_cpu(cpu) {
      /** Don't need to disable preemption here because we have the write lock **/
      replication_stats_t * stats = per_cpu_ptr(&replication_stats_per_core, cpu);
      memset(stats, 0, sizeof(replication_stats_t));
   }

   write_unlock(&reset_stats_rwl);
   return count;
}


static const struct file_operations replication_stats_handlers = {
   .owner   = THIS_MODULE,
   .open    = replication_stats_open,
   .read    = seq_read,
   .llseek  = seq_lseek,
   .release = seq_release,
   .write   = ibs_proc_write,
};

/** END: procfs stuff **/
#endif

static int __init replicate_init(void)
{
#if ENABLE_STATS
   int cpu;
#endif
   work_cachep = kmem_cache_create("work_list_item",
         sizeof(struct work_list_item), 32, SLAB_PANIC, NULL);

   work_thread = kthread_run(rep_work_thread, NULL, "repd");
   if(IS_ERR(work_thread))
      DEBUG_PANIC("repd creation failed\n");

#if ENABLE_STATS
   for_each_online_cpu(cpu) {
      /** We haven't disable premption here but I think that's not a big deal because it's during the initalization **/
      replication_stats_t * stats = per_cpu_ptr(&replication_stats_per_core, cpu);
      memset(stats, 0, sizeof(replication_stats_t));
   }

   if(!proc_create(PROCFS_REPLICATE_STATS_FN, S_IRUGO, NULL, &replication_stats_handlers)){
      DEBUG_WARNING("Cannot create /proc/%s\n", PROCFS_REPLICATE_STATS_FN);
      return -ENOMEM;
   }
#endif

	return 0;
}

void stop_replication_thread(void) {
   if(work_thread && current != work_thread) {
      work_thread = NULL;
   }
}


static inline int clear_single_pte(pgd_t* pgd, struct mm_struct * mm, struct vm_area_struct* vma, unsigned long address)
{
   pte_t *pte;
   int ret = 0;

   pte = get_pte_from_va (pgd, address);
   if(pte) {
      struct page * page = pte_page(*pte);
      pte_t new_pte;

      new_pte = mk_pte(page, PAGE_NONE);
      set_pte_at_notify(mm, address, pte, new_pte);
      ClearPageCollapsed(page);

      ret = 1;
   }

   return ret;
}

/** pgd is a valid pgd for the page **/
int revert_replication(struct mm_struct * mm, struct vm_area_struct * vma, unsigned long address, pte_t * master_pte, struct page * uptodate_page) {
   pte_t new_pte;
   struct page * master_page;

   DEBUG_REPTHREAD("Fixing the ping pong effect for page 0x%lx...\n", page_va(address));
   master_page = pte_page(*master_pte);

   if(uptodate_page != master_page) {
      /** Make sure that the master's page contains the latest copy of the data **/
      copy_user_highpage(master_page, uptodate_page, address, vma);
   }

   /** Unprotect the page on the master **/
   new_pte = mk_pte(master_page, vma->vm_page_prot);
   set_pte_at(mm, address, master_pte, new_pte);

   /** Page is not replicated anymore **/
   ClearPageCollapsed(master_page);
   ClearPageReplication(master_page);

   /** Clear the copies on each node -- pte will be filled lazily **/
   clear_flush_all_node_copies(mm, vma, address);

   INCR_REP_STAT_VALUE(nr_replicated_decisions_reverted, 1);
   return 0;
}

int find_and_revert_replication(struct mm_struct * mm, struct vm_area_struct * vma, unsigned long address, pte_t * master_pte) {
   struct page * page = pte_page(*master_pte);

   if(! PageCollapsed(page)) {
      /* Check slaves */
      int cur_node;
      pte_t * node_pte;

      for_each_online_node(cur_node) {
         /** We check if the entry exists in this node **/
         node_pte = get_pte_from_va(current->mm->pgd_node[cur_node], address);
         if(unlikely(!node_pte)) {
            DEBUG_PANIC("In the current implementation, that should not be the case (address = 0x%lx) !\n", page_va(address));
         }

         page = pte_page(*node_pte);

         if(unlikely(!page)) {
            DEBUG_PANIC("In the current implementation, that should not be the case (address = 0x%lx) !\n", page_va(address));
         }

         if(PageCollapsed(page) || !(pte_flags(*node_pte) & _PAGE_PROTNONE)) {
            // This page is up to date
            break;
         }

         page = NULL;
      }
   }

   if(unlikely(!page)) {
      DEBUG_PANIC("Should not happen !\n");
   }

   return revert_replication(mm, vma, address, master_pte, page);
}

int collapse_all_other_copies (struct mm_struct * mm, struct vm_area_struct * vma, unsigned long address, struct page * my_page, int my_node, pte_t * my_pte) {
   int node = 0;
   pte_t new_pte;

   /** We need to evict the copies from the master and the other slaves if it exists **/
   if(node != -1) {
      DEBUG_PGFAULT("Collapsing the master copy of page 0x%lx\n", address);
      clear_single_pte(mm->pgd_master, mm, vma, address);
   }

   for_each_online_node(node) {
      if(node == my_node) { // We don't want to clear the entry in our mm
         continue;
      }

      /** We check if the entry exists in this node **/
      DEBUG_PGFAULT("Collapsing the node %d copy of page 0x%lx\n", node, address);
      clear_single_pte(mm->pgd_node[node], mm, vma, address);
   }

   /** That's the only good version of the page ... **/
   SetPageCollapsed(my_page);

   /** ... So we can remove the protection **/
   new_pte = pte_mkwrite(*my_pte);
   set_pte_at(mm, address, my_pte, new_pte);
   flush_tlb_page(vma, address);

   return 0;
}

static int dup_page_table(struct mm_struct *mm, pgd_t *pgd_src_base, pgd_t *pgd_dest_base)
{
   struct vm_area_struct *vma;

   for(vma = mm->mmap; vma; vma = vma->vm_next) {
      unsigned long addr;

      for(addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
         pte_t *pte_src=NULL, *pte_dest=NULL;
         pud_t *pud_dest=NULL;
         pmd_t *pmd_dest=NULL;
         pgd_t *pgd_dest=NULL;

         spinlock_t *ptl_src = NULL;

         pte_src = get_locked_pte_from_va (pgd_src_base, mm, addr, &ptl_src);

         if(pte_src) {
            pte_unmap_unlock(pte_src, ptl_src);

            pgd_dest = rep_pgd_offset(pgd_dest_base, addr);
            pud_dest = pud_alloc(mm, pgd_dest, addr);
            if(!pud_dest)
               DEBUG_PANIC("OOM error\n");
            pmd_dest = pmd_alloc(mm, pud_dest, addr);
            if(!pmd_dest)
               DEBUG_PANIC("OOM error\n");
            pte_dest = pte_alloc_map(mm, vma, pmd_dest, addr);
            if(!pte_dest)
               DEBUG_PANIC("OOM error\n");

            pte_src = get_locked_pte_from_va (pgd_src_base, mm, addr, &ptl_src);
            if(unlikely(!pte_src)) {
               DEBUG_PANIC("Wow !\n");
            }

            set_pte_at_notify(mm, addr, pte_dest, *pte_src);
            pte_unmap_unlock(pte_src, ptl_src);
         }
      }
   }

   return 0;
}

#define BUF_LGTH 512
int dump_pgd_content(struct mm_struct *mm) {
   struct vm_area_struct *vma;
   printk("Dumping the content of mm (%lu pages)...\n", mm->total_vm);

   for(vma = mm->mmap; vma; vma = vma->vm_next) {
      unsigned long addr;
      unsigned long nr_pages = (vma->vm_end - vma->vm_start) / PAGE_SIZE;

      printk("-- VMA [0x%lx ; 0x%lx[, size: %lu pages", vma->vm_start, vma->vm_end, nr_pages);
      if(vma->vm_file){
         char buf[BUF_LGTH];
         char * path = d_path(&vma->vm_file->f_path, buf, BUF_LGTH);
         if (!IS_ERR(path)) {
            printk (" [file = %s]\n", path);
         }
         else {
            printk (" [file = UNKNOWN]\n");
         }
      }
      else {
         if (vma->vm_start <= mm->brk && vma->vm_end >= mm->start_brk) {
            printk(" [heap]");
         } else if (vma->vm_start <= mm->start_stack && vma->vm_end >= mm->start_stack) {
            printk(" [stack]");
         }
         printk("\n");
      }

      for(addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
         pte_t *pte_master=NULL;
         pte_master = get_pte_from_va (mm->pgd_master, addr);

         if(pte_master) {
            struct page * page_master = vm_normal_page(vma, addr, *pte_master);
            printk("\tva = 0x%lx ",  addr);

            if (page_master) {
               if(!PageReplication(page_master)) {
                  printk("---> pa = 0x%lx",  (long unsigned) page_address(page_master));
               }
               else {
                  int node = 0;

                  if(PageCollapsed(page_master)) {
                     printk("---> (+master = 0x%lx, nodes = [ ", (long unsigned) page_address(page_master));
                  }
                  else if (pte_flags(*pte_master) & _PAGE_PROTNONE) {
                     printk("---> (-master = 0x%lx, nodes = [ ", (long unsigned) page_address(page_master));
                  }
                  else {
                     printk("---> (master = 0x%lx, nodes = [ ", (long unsigned) page_address(page_master));
                  }

                  for_each_online_node(node) {
                     pte_t *pte_node=NULL;
                     struct page * page_node=NULL;

                     pte_node = get_pte_from_va (mm->pgd_node[node], addr);
                     if(pte_node) {
                        unsigned long va_addr = 0;
                        page_node = vm_normal_page(vma, addr, *pte_node);

                        if(page_node) {
                           va_addr = (unsigned long) page_address(page_node);

                           if(PageCollapsed(page_node)) {
                              printk("+");
                           }
                           else if (pte_flags(*pte_node) & _PAGE_PROTNONE) {
                              printk("-");
                           }
                        }
                        printk("0x%lx ", va_addr);
                     }
                     else {
                        printk("--- ");
                     }

                     if(node == num_online_nodes() -1) {
                        printk("] <REPLICATED>");
                     }
                  }
               }
            }

/*            if(pte_none(*pte_master))
               printk(" <NONE>");

            if(! pte_present(*pte_master))
               printk(" <!PRESENT>");
*/

            if(pte_special(*pte_master))
               printk(" <SPECIAL>");

            if((pte_flags(*pte_master) & _PAGE_PROTNONE))
               printk(" <!RW>");

            else if (! pte_write(*pte_master))
               printk(" <!WRITE>");

            printk("\n");
         }
      }
   }

   return 0;
}

int check_pgd_consistency(struct mm_struct *mm) {
   struct vm_area_struct *vma;
   int node;
   pte_t *pte_master;
   spinlock_t *ptl_master = NULL;

   if(!is_replicated(mm)) {
      DEBUG_WARNING("Don't need to check the consistency if the pgd has not been replicated!\n");
      return 0;
   }

   for(vma = mm->mmap; vma; vma = vma->vm_next) {
      unsigned long addr;
      for(addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
         struct page *master_page = NULL;

         int nb_valid_copies = 0;
         int nb_collapsed_copies = 0;
         int counted = 0;

         pte_master = get_locked_pte_from_va (mm->pgd_master, mm, addr, &ptl_master);
         if(pte_master) {
            master_page = vm_normal_page(vma, addr, *pte_master);

            if(master_page && PageReplication(master_page) && (vma->vm_file)) {
               DEBUG_WARNING("A replicated page [0x%lx] should not be in a file\n", addr);
               goto fail;
            }
         }

         for_each_online_node(node) {
            struct page *node_page = NULL;
            pte_t *pte_node = NULL;

            pte_node = get_pte_from_va (mm->pgd_node[node], addr);

            if(pte_node && !pte_master) {
               DEBUG_WARNING("Found a pte in node %d but not in the master for address 0x%lx\n", node, addr);
               goto fail_nolock;
            }

            if(!pte_node) {
               if(master_page && PageReplication(master_page)) {
                  DEBUG_WARNING("Replicated page [0x%lx]. A pte should exist on node %d (and be present)\n", addr, node);
                  goto fail;
               }
               continue;
            }

            if(pte_special(*pte_master) && pte_special(*pte_node)) {
               continue;
            }

            if(pte_special(*pte_master) || pte_special(*pte_node)) {
               DEBUG_WARNING("Master [%d] and %d [%d] - Only one of them is special !\n", pte_special(*pte_master), node, pte_special(*pte_node));
               goto fail;
            }

            node_page = vm_normal_page(vma, addr, *pte_node);

            if((master_page && !node_page) || (!master_page && node_page)) {
               DEBUG_WARNING("Master [%p] and %d [%p] - Inconsistent pages\n", master_page, node, node_page);
               goto fail;
            }

            if (master_page && node_page) {
               if((PageReplication(master_page) && !PageReplication(node_page)) || (!PageReplication(master_page) && PageReplication(node_page))) {
                  DEBUG_WARNING("Master [%d] and %d [%d] - Inconsistent replication state\n", PageReplication(master_page), node, PageReplication(node_page));
                  goto fail;
               }

               if(PageReplication(master_page) && (node_page == master_page)) {
                  DEBUG_WARNING("Master and %d - Page 0x%lx marked as replicated but pages are the same\n", node, addr);
                  goto fail;
               }

               if(!PageReplication(master_page) && (node_page != master_page)) {
                  DEBUG_WARNING("Master and %d - Page 0x%lx not marked as replicated but pages are different (pa_master = %p, pa = %p)\n", node, addr, page_address(master_page), page_address(node_page));
                  goto fail;
               }

               if(!counted && PageReplication(master_page)) {
                  if(PageCollapsed(master_page)) {
                     nb_collapsed_copies++;
                     nb_valid_copies++;
                  }
                  counted = 1;
               }

               if(PageReplication(node_page)) {
                  if(PageCollapsed(node_page)) {
                     nb_collapsed_copies++;
                     nb_valid_copies++;
                  }
                  else if (!(pte_flags(*pte_node) & _PAGE_PROTNONE)) {
                     nb_valid_copies++;
                  }
               }
            }
         }

         if(master_page) {
            if(PageReplication(master_page)) {
               if(nb_valid_copies == 0 || (nb_valid_copies > 1 && nb_collapsed_copies > 0) || nb_collapsed_copies > 1) {
                  DEBUG_WARNING("Replication: inconsistent state on page 0x%lx (nb_collapsed_copies = %d, nb_valid_copies = %d)\n", addr, nb_collapsed_copies, nb_valid_copies);
                  goto fail;
               }
            }
         }

         if(pte_master) {
            pte_unmap_unlock(pte_master, ptl_master);
         }
      }
   }
   return 0;

fail:
   pte_unmap_unlock(pte_master, ptl_master);
fail_nolock:
   dump_pgd_content(mm);
   return 1;
}

module_init(replicate_init)
