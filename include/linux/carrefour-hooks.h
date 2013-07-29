#ifndef __LINUX_CARREFOUR_HOOKS_H
#define __LINUX_CARREFOUR_HOOKS_H

struct carrefour_options_t {
   int page_bouncing_fix_4k;
   int page_bouncing_fix_2M;
   int use_balance_numa_api;
   int use_balance_numa_rate_limit;
   int sync_thp_migration;
   int async_4k_migrations;
};
extern struct carrefour_options_t carrefour_options;

struct carrefour_hook_stats_t {
   u64 time_spent_in_migration;
   u64 s_migrate_nb_calls;

   u64 time_spent_in_split;
   u64 split_nb_calls;
};
extern struct carrefour_hook_stats_t carrefour_hook_stats;

// These are our custom errors
#define EPAGENOTFOUND   65
#define EREPLICATEDPAGE 66
#define EINVALIDPAGE    67
#define ENOTMISPLACED   68
#define EBOUNCINGFIX    69

// Returns 0 if the page is present, -<error> otherwise
// If the page is a regular huge page huge = 1, huge = 2 if it is a THP, huge = 0 otherwise
int page_status_for_carrefour(int pid, unsigned long addr, int * alread_treated, int * huge);

// -1 if the address is invalid, 0 if regular, 1 if trans_huge
int is_huge_addr_sloppy (int pid, unsigned long addr);

int s_migrate_pages(pid_t pid, unsigned long nr_pages, void ** pages, int * nodes);
int s_migrate_hugepages(pid_t pid, unsigned long nr_pages, void ** pages, int * nodes);
int find_and_migrate_thp(int pid, unsigned long addr, int to_node);

// Returns 0 if we found and splitted a huge page
int find_and_split_thp(int pid, unsigned long addr);

int move_thread_to_node(pid_t tid, int node);
struct task_struct * get_task_struct_from_pid(int pid);

int is_valid_pid(int pid);

void reset_carrefour_hooks(void);
void reset_carrefour_stats(void);
void configure_carrefour(struct carrefour_options_t options);
struct carrefour_hook_stats_t* get_carrefour_hook_stats(void);

#endif
