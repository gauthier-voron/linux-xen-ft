#ifndef __LINUX_REPLICATE_H
#define __LINUX_REPLICATE_H
/* JRF */
#include <linux/bitops.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/sched.h>

/** Configuration of replication internal stuff **/
#include <linux/carrefour-stats.h>
#include <linux/replicate-options.h>

/* Modeled after pgd_offset in pgtable.h */
#define rep_pgd_offset(pgd, address) (pgd + pgd_index(address))

/** Function headers **/
int replicate_madvise(pid_t tgid, unsigned long start, unsigned long len, int advice);

/** Copy the same page from the src mm to the dest mm**/
int rep_copy_pgd_pte(struct mm_struct* mm, struct vm_area_struct * vma, pgd_t * src, pgd_t *dest, unsigned long address);

/**
 * It takes an address unmapes the corresponding page from the mm
**/
int find_and_revert_replication(struct mm_struct * mm, struct vm_area_struct * vma, unsigned long address, pte_t * master_pte);
int revert_replication(struct mm_struct * mm, struct vm_area_struct * vma, unsigned long address, pte_t * master_pte, struct page * uptodate_page, int count_it);
int collapse_all_other_copies (struct mm_struct * mm, struct vm_area_struct * vma, unsigned long address, struct page * my_page, int my_node, pte_t * my_pte);
void clear_flush_all_node_copies (struct mm_struct * mm, struct vm_area_struct * vma, unsigned long address);

int check_pgd_consistency(struct mm_struct *mm);
int dump_pgd_content(struct mm_struct *mm);
void stop_replication_thread(void);

void print_pg_fault (unsigned long address, int write, struct vm_area_struct* vma);

/** Variables **/
extern struct task_struct * work_thread;

/** Errors **/
#define EPID_NOTFOUND            (-200)
#define EADDRESS_INVALID         (-201)
#define EADDRESS_NOT_SUPPORTED   (-202)
#define EREPD_NOT_RUNNING        (-203)

/** Useful macros (not sure you really want to read this) **/
#define is_replicated(mm)  (mm && mm->replicated_mm)
#define is_master_pgd(mm, pgd) (mm && (pgd == (mm)->pgd_master))
#define page_va(address)   (((address) >> PAGE_SHIFT) << PAGE_SHIFT)
#define is_user_addr(addr) ((unsigned long) addr <= TASK_SIZE)

#define __DEBUG(msg, args...)       printk(KERN_DEBUG "[Core %2d, TID %5d, %25.25s, %20.20s:%4d] " msg, smp_processor_id(), current->pid, __FUNCTION__, __FILE__, __LINE__, ##args)
#define DEBUG_WARNING(msg, args...) printk(KERN_DEBUG "[Core %2d, TID %5d, %25.25s, %20.20s:%4d] (WARNING) " msg, smp_processor_id(), current->pid, __FUNCTION__, __FILE__, __LINE__, ##args)

#define DEBUG_PANIC(msg, args...) { \
   DEBUG_WARNING(msg, ##args); \
   stop_replication_thread(); \
   BUG_ON(1); \
}


#if VERBOSE_PGFAULT
#define DEBUG_PGFAULT(msg, args...) { \
   if(work_thread && is_replicated(current->mm)) { \
      __DEBUG("< 0x%lx, 0x%lx > " msg, address, page_va(address), ##args); \
   } \
}
#else
#define DEBUG_PGFAULT(msg, args...) do {} while(0);
#endif

#if VERBOSE_REPTHREAD
#define DEBUG_REPTHREAD(msg, args...) if(work_thread) { __DEBUG("(REPTHREAD) " msg, ##args); }
#else
#define DEBUG_REPTHREAD(msg, args...) do {} while(0);
#endif

#if VERBOSE_OTHERS
#define DEBUG_REP_VV(msg, args...) { \
   if(work_thread && is_replicated(current->mm)) { \
      __DEBUG(msg, ##args); \
   } \
}
#define DEBUG_PRINT(msg, args...)  __DEBUG(msg, ##args)
#else
#define DEBUG_REP_VV(msg, args...) do {} while (0)
#define DEBUG_PRINT(msg, args...) do {} while (0)
#endif

/**
* Utility functions
* They are here and not in replicate.c for performance (maybe that's a bad reason)
**/
static inline pte_t* get_locked_pte_from_va (pgd_t* pgd, struct mm_struct * mm,
                        unsigned long address, spinlock_t** ptl) {
   pte_t * pte = NULL;

   pgd = rep_pgd_offset(pgd, address);
   if (pgd_present(*pgd )) {
      pud_t *pud = pud_offset(pgd, address);
      if(pud_present(*pud)) {
         pmd_t *pmd = pmd_offset(pud, address);
         if (pmd_present(*pmd ) && !pmd_trans_huge(*pmd)) {
            pte = pte_offset_map_lock(mm, pmd, address, ptl);
            if (! pte_present(*pte)) {
               pte_unmap_unlock(pte, *ptl);
               pte = NULL;
            }
         }
      }
   }

   return pte;
}

static inline pte_t* get_pte_from_va (pgd_t* pgd, unsigned long address) {
   pte_t * pte = NULL;

   pgd = rep_pgd_offset(pgd, address);
   if (pgd_present(*pgd )) {
      pud_t *pud = pud_offset(pgd, address);
      if(pud_present(*pud)) {
         pmd_t *pmd = pmd_offset(pud, address);
         if (pmd_present(*pmd )) {
            pte = pte_offset_map(pmd, address);
            if (! pte_present(*pte)) {
               pte = NULL;
            }
         }
      }
   }

   return pte;
}


static inline unsigned long get_pa_from_va (pgd_t * pgd, struct vm_area_struct * vma, unsigned long address) {
   unsigned long pa = 0;
   pgd = rep_pgd_offset(pgd, address);
   if (pgd_present(*pgd )) {
      pud_t *pud = pud_offset(pgd, address);
      if(pud_present(*pud)) {
         pmd_t *pmd = pmd_offset(pud, address);
         if (pmd_present(*pmd )) {
            pte_t *pte = pte_offset_map(pmd, address);
            if (pte_present(*pte)) {
               pa = (long unsigned) page_address(pte_page(*pte));
            }
         }
      }
   }

   return pa;
}

#endif
