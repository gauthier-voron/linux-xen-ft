#ifndef __LINUX_CARREFOUR_HOOKS_H
#define __LINUX_CARREFOUR_HOOKS_H

// Returns 0 if the page is present, -1 otherwise
// If the page is a regular huge page huge = 1, huge = 2 if it is a THP, huge = 0 otherwise
int page_status_for_carrefour(int pid, unsigned long addr, int * alread_treated, int * huge);
int s_migrate_pages(pid_t pid, unsigned long nr_pages, void ** pages, int * nodes);

int move_thread_to_node(pid_t tid, int node);
struct task_struct * get_task_struct_from_pid(int pid);

int is_valid_pid(int pid);

void reset_carrefour_hooks(void);
void reset_carrefour_stats(void);

struct carrefour_options_t {
   int page_bouncing_fix;
};

struct carrefour_hook_stats_t {
   unsigned real_nb_migrations;
   unsigned migr_from_to_node[MAX_NUMNODES][MAX_NUMNODES];
   u64 time_spent_in_migration;
};

#endif
