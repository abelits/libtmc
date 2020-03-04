#ifndef __ISOL_INTERNALS_H__
#define __ISOL_INTERNALS_H__

/*
 * Task isolation support.
 *
 * By Alex Belits <abelits@marvell.com>
 */

#include <stdlib.h>
#include <pthread.h>

/* Request types */
enum memipc_req_type
    {
	MEMIPC_REQ_NONE,
	MEMIPC_REQ_INIT,
	MEMIPC_REQ_START_READY,
	MEMIPC_REQ_START_LAUNCH,
	MEMIPC_REQ_START_LAUNCH_DONE,
	MEMIPC_REQ_START_LAUNCH_FAILURE,
	MEMIPC_REQ_START_CONFIRMED,
	MEMIPC_REQ_TERMINATE,
	MEMIPC_REQ_EXIT_ISOLATION,
	MEMIPC_REQ_EXITING,
	MEMIPC_REQ_LEAVE_ISOLATION,
	MEMIPC_REQ_OK_LEAVE_ISOLATION,
	MEMIPC_REQ_PING,
	MEMIPC_REQ_PONG,
	MEMIPC_REQ_CMD,
	MEMIPC_REQ_PRINT
    };

struct memipc_area;
struct memipc_thread_params;

/*
 * Create an area descriptor and allocate the area.
 */
struct memipc_area *memipc_area_create(size_t size, size_t map_size,
				       size_t offset, int fd,
				       unsigned char *ptr);

/*
 * Create a duplicate area descriptor for threaded model.
 *
 * This is necessary because all indexes and counters are supposed to
 * be private to the reader and writer threads or processes while
 * buffer itself is shared. So the same buffer is visible from those
 * threads through two initially identical descriptors.
 *
 * The mechanism will not work properly if area descriptors are shared
 * between threads.
 */
struct memipc_area *memipc_area_dup(struct memipc_area *src);

/*
 * Delete area and its descriptor.
 */
void memipc_area_delete(struct memipc_area *area);

/*
 * Delete area descriptor that was created as a duplicate.
 *
 * There is no reference counting in this mechanism, caller is
 * responsible to make sure that one of the two descriptors is deleted
 * with memipc_area_delete() and another with
 * memipc_area_delete_duplicate().
 */
void memipc_area_delete_duplicate(struct memipc_area *area);

/*
 * Create a request in a given area.
 */
int memipc_add_req(struct memipc_area *area, enum memipc_req_type req_type,
		   ssize_t req_size, unsigned char *req_data);

/*
 * Get request from a given area.
 */
int memipc_get_req(struct memipc_area *area, enum memipc_req_type *req_type,
		   ssize_t *req_size, unsigned char *req_data);

/*
 *  Call this function in the main loop of the slave/managed thread.
 */
int memipc_thread_pass(struct memipc_thread_params *params);

/*
 *  Same as above, except it looks for the current thread.
 */
int memipc_thread_pass_default(void);

/*
 * Notify the manager about thread exit. This function must be called before
 * thread exit.
 */
void memipc_isolation_announce_exit(void);

/*
 * printf() replacement for isolated mode. Will return a negative number if
 * there is not enough space in buffer, retry if necessary.
 */
int memipc_isolation_printf(const char *fmt, ...);

/*
 * Handler for requests in a master/manager thread.
 */
void memipc_master_handle_request(int read_req_type,
				  ssize_t read_req_size,
				  unsigned char *memipc_read_buffer,
				  struct memipc_thread_params *thread);

/*
 * Claim given CPU, or any CPU.
 *
 * Negative argument means, the first available CPU.
 * Returns a thread descriptor or NULL if not available.
 */
struct memipc_thread_params *isolation_claim_cpu(int cpu);

/*
 * Release CPU of a given thread.
 *
 * This function should be only called once on a thread with claimed
 * CPU.
 */
void isolation_release_cpu(struct memipc_thread_params *thread);

/*
 * Get the number of threads that can run isolated.
 */
int memipc_isolation_get_max_isolated_threads_count(void);

/*
 * Terminate thread.
 */
void memipc_isolation_terminate_thread(struct memipc_thread_params *thread);

/*
 * Terminate all threads.
 */
void memipc_isolation_terminate_all_threads(void);

/*
 * Manager loop.
 */
int memipc_isolation_run_threads(void);

/*
 * Claim a CPU, then start a thread on it.
 *
 * Initial or manager thread can call this function.
 *
 * This is one way of starting a managed thread -- it appears under
 * managed environment from the very beginning. Alternatively thread
 * can be started independently, then claim a CPU.
 */
int isolation_thread_create(int cpu, const pthread_attr_t *attr,
			    void *(*init_routine)(void*),
			    void *(*start_routine)(void*), void *arg);

/*
 * Claim a CPU from a started thread.
 *
 * Thread may call this function.
 *
 * This is another way of starting a managed thread -- already existing thread
 * is attached to the managed environment. This can not be done in a thread
 * that is already managed.
 */
int isolation_connect_this_thread(int cpu);

/*
 * Send request to the manager to run this thread isolated.
 */
int isolation_request_launch_this_thread(volatile int *c);

/*
 * Initialize environment for a given CPU list.
 */
int memipc_isolation_initialize_cpulist(const char *cpulist);

/*
 * Initialize environment for all CPUs available for task isolation.
 */
int memipc_isolation_initialize(void);

/*
 * Find descriptor of a managed thread with a given ID.
 */
struct memipc_thread_params *isolation_find_thread(pthread_t thread_id);

#endif
