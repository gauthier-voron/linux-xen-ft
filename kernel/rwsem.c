/* kernel/rwsem.c: R/W semaphores, public implementation
 *
 * Written by David Howells (dhowells@redhat.com).
 * Derived from asm-i386/semaphore.h
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/export.h>
#include <linux/rwsem.h>

#include <linux/atomic.h>

// FGAUD
#include <linux/carrefour-stats.h>
#include <linux/kallsyms.h>

/*
 * lock for reading
 */
void __sched down_read(struct rw_semaphore *sem)
{
#if ENABLE_RWLOCK_STATS
	char caller_name[KSYM_NAME_LEN];
#endif
   RECORD_DURATION_START;

	might_sleep();
   DEBUG_SEM_LOCKS(0, 0);

	rwsem_acquire_read(&sem->dep_map, 0, 0, _RET_IP_);
	LOCK_CONTENDED(sem, __down_read_trylock, __down_read);

   DEBUG_SEM_LOCKS(1, 0);
   RECORD_DURATION_END(time_spent_acquiring_readlocks, nr_readlock_taken);

#if ENABLE_RWLOCK_STATS
	if(likely(kallsyms_lookup((unsigned long) __builtin_return_address(0), NULL, NULL, NULL, caller_name))) {
		record_fn_call(caller_name,"-rdlock", (rdt_stop - rdt_start));
	}
	else {
		record_fn_call("UNKNOWN",NULL, (rdt_stop - rdt_start));
	}
#endif

#if ENABLE_TSK_MIGRATION_STATS 
	if(current) {
		current->is_in_rw_lock++;
	}
#endif

#if ENABLE_RWSEM_ORDER_HACK
	if(sem->nr_read_locks && sem->nr_read_locks_index < NR_READ_LOCK_SIZE) {
		__sync_fetch_and_add(&sem->nr_read_locks[sem->nr_read_locks_index], 1);
	}
#endif
}

EXPORT_SYMBOL(down_read);

/*
 * trylock for reading -- returns 1 if successful, 0 if contention
 */
int down_read_trylock(struct rw_semaphore *sem)
{
   int ret;
   RECORD_DURATION_START;

	ret = __down_read_trylock(sem);
	if (ret == 1) {
		rwsem_acquire_read(&sem->dep_map, 0, 1, _RET_IP_);
   
      DEBUG_SEM_LOCKS(1, 0);
      RECORD_DURATION_END(time_spent_acquiring_readlocks, nr_readlock_taken);

#if ENABLE_TSK_MIGRATION_STATS 
		if(current) {
			current->is_in_rw_lock++;
		}
#endif

#if ENABLE_RWSEM_ORDER_HACK
		if(sem->nr_read_locks && sem->nr_read_locks_index < NR_READ_LOCK_SIZE) {
			__sync_fetch_and_add(&sem->nr_read_locks[sem->nr_read_locks_index], 1);
		}
#endif
   }

	return ret;
}

EXPORT_SYMBOL(down_read_trylock);

/*
 * lock for writing
 */

void __sched down_write(struct rw_semaphore *sem)
{
#if ENABLE_RWLOCK_STATS
	char caller_name[KSYM_NAME_LEN];
#endif

   RECORD_DURATION_START;

	might_sleep();

   DEBUG_SEM_LOCKS(0, 1);

	rwsem_acquire(&sem->dep_map, 0, 0, _RET_IP_);
	LOCK_CONTENDED(sem, __down_write_trylock, __down_write);

   DEBUG_SEM_LOCKS(1, 1);
   RECORD_DURATION_END(time_spent_acquiring_writelocks, nr_writelock_taken);

#if ENABLE_RWLOCK_STATS
	if(likely(kallsyms_lookup((unsigned long) __builtin_return_address(0), NULL, NULL, NULL, caller_name))) {
		record_fn_call(caller_name,"-wrlock", (rdt_stop - rdt_start));
	}
	else {
		record_fn_call("UNKNOWN",NULL, (rdt_stop - rdt_start));
	}
#endif

#if ENABLE_TSK_MIGRATION_STATS 
	if(current) {
		current->is_in_rw_lock++;
	}
#endif

#if ENABLE_RWSEM_ORDER_HACK
	if(sem->nr_read_locks && sem->nr_read_locks_index < NR_READ_LOCK_SIZE) {
		sem->nr_read_locks_index++;
	}
#endif
}

EXPORT_SYMBOL(down_write);

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
int down_write_trylock(struct rw_semaphore *sem)
{
   int ret;
   RECORD_DURATION_START;

	ret = __down_write_trylock(sem);

	if (ret == 1) {
		rwsem_acquire(&sem->dep_map, 0, 1, _RET_IP_);

      DEBUG_SEM_LOCKS(1, 1);
      RECORD_DURATION_END(time_spent_acquiring_writelocks, nr_writelock_taken);

#if ENABLE_TSK_MIGRATION_STATS 
		if(current) {
			current->is_in_rw_lock++;
		}
#endif

#if ENABLE_RWSEM_ORDER_HACK
		if(sem->nr_read_locks && sem->nr_read_locks_index < NR_READ_LOCK_SIZE) {
			sem->nr_read_locks_index++;
		}
#endif
   }
	return ret;
}

EXPORT_SYMBOL(down_write_trylock);

/*
 * release a read lock
 */
void up_read(struct rw_semaphore *sem)
{
	rwsem_release(&sem->dep_map, 1, _RET_IP_);
	__up_read(sem);
   
   DEBUG_SEM_LOCKS(2, 0);

#if ENABLE_TSK_MIGRATION_STATS 
	if(current) {
		current->is_in_rw_lock--;
	}
#endif
}

EXPORT_SYMBOL(up_read);

/*
 * release a write lock
 */
void up_write(struct rw_semaphore *sem)
{
	rwsem_release(&sem->dep_map, 1, _RET_IP_);
	__up_write(sem);

   DEBUG_SEM_LOCKS(2, 1);

#if ENABLE_TSK_MIGRATION_STATS 
	if(current) {
		current->is_in_rw_lock--;
	}
#endif
}

EXPORT_SYMBOL(up_write);

/*
 * downgrade write lock to read lock
 */
void downgrade_write(struct rw_semaphore *sem)
{
	/*
	 * lockdep: a downgraded write will live on as a write
	 * dependency.
	 */
	__downgrade_write(sem);
}

EXPORT_SYMBOL(downgrade_write);

#ifdef CONFIG_DEBUG_LOCK_ALLOC

void down_read_nested(struct rw_semaphore *sem, int subclass)
{
	might_sleep();
	rwsem_acquire_read(&sem->dep_map, subclass, 0, _RET_IP_);
	LOCK_CONTENDED(sem, __down_read_trylock, __down_read);
}

EXPORT_SYMBOL(down_read_nested);

void _down_write_nest_lock(struct rw_semaphore *sem, struct lockdep_map *nest)
{
	might_sleep();
	rwsem_acquire_nest(&sem->dep_map, 0, 0, nest, _RET_IP_);

	LOCK_CONTENDED(sem, __down_write_trylock, __down_write);
}

EXPORT_SYMBOL(_down_write_nest_lock);

void down_write_nested(struct rw_semaphore *sem, int subclass)
{
	might_sleep();
	rwsem_acquire(&sem->dep_map, subclass, 0, _RET_IP_);
	LOCK_CONTENDED(sem, __down_write_trylock, __down_write);
}

EXPORT_SYMBOL(down_write_nested);

#endif
