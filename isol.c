/*
 * Task isolation support.
 *
 * By Alex Belits <abelits@marvell.com>
 *
 * This is the userspace part of the task isolation support for Linux.
 * It requires task isolation patch applied to he kernel. That patch
 * is based on the task isolation patch v15 by Chris Metcalf
 * <cmetcalf@mellanox.com> ported to kernel 4.9.x, plus the update
 * that disables timers while isolated tasks are running.
 *
 * The kernel configuration requires the following:
 *
 * Should be set:
 *
 * CONFIG_TASK_ISOLATION
 * CONFIG_TICK_ONESHOT
 * CONFIG_NO_HZ_COMMON
 * CONFIG_NO_HZ_FULL
 * CONFIG_HIGH_RES_TIMERS
 * CONFIG_RCU_NOCB_CPU
 *
 * Should NOT be set:
 *
 * CONFIG_NO_HZ_FULL_SYSIDLE
 * CONFIG_HZ_PERIODIC
 * CONFIG_NO_HZ_IDLE
 * CONFIG_NO_HZ_FULL_SYSIDLE
 * CONFIG_NO_HZ
 *
 * May be set for convenience:
 *
 * CONFIG_TASK_ISOLATION_ALL
 * CONFIG_NO_HZ_FULL_ALL
 * CONFIG_RCU_NOCB_CPU_ALL
 *
 * Please use "make menuconfig" and provided examples to create
 * compatible kernel configuration. Use the list above to verify the
 * configuration results.
 *
 *
 * This is the thread-based implementation, intended to run the
 * manager and all isolated tasks as threads within a single process.
 *
 */

/*
  Some functions may ignore parameters intended for future functionality.
  This is intentional.
*/
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-result"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <dirent.h>
#include <sched.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/prctl.h>

/* Internal functions */
#include "isol-internals.h"

/* Server */
#include "isol-server.h"

/* TMC for ODP */
#include <tmc/isol.h>

/* Compile-time options for debugging output */

/* Enable message on startup */
#ifndef DEBUG_ISOL_STARTUP_MESSAGE
#define DEBUG_ISOL_STARTUP_MESSAGE 0
#endif

/* Show all timers for debugging */
#ifndef DEBUG_ISOL_ALWAYS_SHOW_ALL_TIMERS
#define DEBUG_ISOL_ALWAYS_SHOW_ALL_TIMERS 0
#endif

/* List all timer entries, not just high resolution timers. */
#ifndef DEBUG_ISOL_LIST_ALL_TIMER_ENTRIES
#define DEBUG_ISOL_LIST_ALL_TIMER_ENTRIES 0
#endif

/* Maintain and show names for processes and timers. */
#ifndef DEBUG_ISOL_NAMES
#define DEBUG_ISOL_NAMES 0
#endif

/* Verbose debugging messages. */
#ifndef DEBUG_ISOL_VERBOSE
#define DEBUG_ISOL_VERBOSE 0
#endif

/* Compile-time options for monitoring implementation. */

/* Two monitoring methods are supported */
#if (!defined(ISOLATION_MONITOR_IN_MASTER)	\
     && !defined(ISOLATION_MONITOR_IN_SLAVE))
#define ISOLATION_MONITOR_IN_MASTER 1
#define ISOLATION_MONITOR_IN_SLAVE 0
#else
#if (defined(ISOLATION_MONITOR_IN_MASTER) \
     && !defined(ISOLATION_MONITOR_IN_SLAVE))
#define ISOLATION_MONITOR_IN_SLAVE (1 - ISOLATION_MONITOR_IN_MASTER)
#else
#if (defined(ISOLATION_MONITOR_IN_SLAVE) \
     && !defined(ISOLATION_MONITOR_IN_MASTER))
#define ISOLATION_MONITOR_IN_MASTER (1 - ISOLATION_MONITOR_IN_SLAVE)
#endif
#endif
#endif

/* Use CPU subsets to support multiple applications. */
#ifndef USE_CPU_SUBSETS
#define USE_CPU_SUBSETS 1
#endif

/*
  CPU subsets file.  Used only if CPU subsets feature is
  enabled. Contains entries in the format:
  <subset name>:<cpu list>
  ex:
  1:1-12
  2:13-23
*/
#if USE_CPU_SUBSETS
#define CPU_SUBSETS_FILE "/etc/cpu_subsets"
#endif

/*
  The following is specific to the patched kernel, and may be
  incompatible with other kernel versions. If the build environment
  includes patched headers (with PR_SET_TASK_ISOLATION defined), the
  values from there will be used.
*/
#ifndef PR_SET_TASK_ISOLATION
#define PR_SET_TASK_ISOLATION          48
#define PR_GET_TASK_ISOLATION          49
#define PR_TASK_ISOLATION_ENABLE       (1 << 0)
#define PR_TASK_ISOLATION_USERSIG      (1 << 1)
#define PR_TASK_ISOLATION_SET_SIG(sig) (((sig) & 0x7f) << 8)
#define PR_TASK_ISOLATION_GET_SIG(bits) (((bits) >> 8) & 0x7f)
#define PR_TASK_ISOLATION_NOSIG \
        (PR_TASK_ISOLATION_USERSIG | PR_TASK_ISOLATION_SET_SIG(0))
#endif

/*
 * IPC mechanism based entirely on shared or common memory.
 *
 * This is a work in progress, so currently it is written for easier
 * debugging, not performance. The interface will mostly remain the
 * same, however some functions will have to be changed to inline and
 * macros to avoid overhead. Internals are subject to change.
 */

/* For now, user and encoded and block sizes will look like this. */
#define SEVEN (7)
#define EIGHT (8)

/* Memory area. */
#define AREA_SIZE (4096)

/*
 * Per-process or per-thread ID.
 * For now, use pthread_t, however it may be changed to pid_t for external
 * manager process.
 */
static __thread pthread_t memipc_my_pid = 0;
static __thread int memipc_thread_launch_confirmed = 0;
__thread int memipc_thread_continue_flag = 1;
__thread int memipc_thread_ok_leave_flag = 0;
static unsigned char newdata_one = 1;
__thread volatile unsigned char *memipc_check_newdata_ptr = &newdata_one;
__thread volatile unsigned char memipc_check_signal = 0;

struct memipc_thread_params;

static __thread struct memipc_thread_params *memipc_thread_self = NULL;
static __thread int memipc_thread_fd = -1;

/* Memory area descriptor. */
struct memipc_area
{
	unsigned char volatile *area;
	unsigned char volatile *wptr;
	unsigned char volatile *rptr;
	size_t size;
	size_t inbuffer;
	pthread_t writer;
	pthread_t reader;
};

/*
 * Request header.
 * Does not exist in memory but assumed in message layout.
 */
struct memipc_req_header
{
	char t;
	uint32_t size;
} __attribute__((packed));


char *memipc_area_name(int cpu)
{
	char *s;
	s = (char *)malloc(37);
	if (s == NULL) return NULL;
	/*               0123456789012345 <- 16 bytes*/
	snprintf(s, 37, "/isol_server_CPU%d", cpu);
	return s;
}

/*
 * Create an area descriptor and allocate the area.
 */
struct memipc_area *memipc_area_create(size_t size, size_t map_size,
				       size_t offset, int fd,
				       unsigned char *ptr)
{
	unsigned char *p;
	struct memipc_area *area;

	area = malloc(sizeof(struct memipc_area));
	if (area == NULL)
		return NULL;

	if (ptr == NULL) {
		p = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd, offset);
		if ( p == MAP_FAILED) {
			free(area);
			return NULL;
		}
	} else
		p = ptr + offset;

	area->area = p;
	area->wptr = p;
	area->rptr = p;
	area->size = size;
	area->inbuffer = 0;
	area->writer = 0;
	area->reader = 0;

	return area;
}

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
struct memipc_area *memipc_area_dup(struct memipc_area *src)
{
	struct memipc_area *dst;
	if (src == NULL)
		return NULL;
	dst = malloc(sizeof(struct memipc_area));
	if (dst == NULL)
		return NULL;
	memcpy(dst, src, sizeof(struct memipc_area));
	return dst;
}

/*
 * Delete area and its descriptor.
 */
void memipc_area_delete(struct memipc_area *area)
{
	if (area == NULL)
		return;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
	munmap((void *)area->area, area->size);
#pragma GCC diagnostic pop

	free(area);
}

/*
 * Delete area descriptor that was created as a duplicate.
 *
 * There is no reference counting in this mechanism, caller is
 * responsible to make sure that one of the two descriptors is deleted
 * with memipc_area_delete() and another with
 * memipc_area_delete_duplicate().
 */
void memipc_area_delete_duplicate(struct memipc_area *area)
{
	if (area == NULL)
		return;
	free(area);
}

/*
 * Write SEVEN bytes encoded as EIGHT bytes.
 *
 * Nonzero return value means that write area is not ready.
 */
static int write_encode_bytes(unsigned char volatile *dst,
			      const unsigned char *src,
			      unsigned int size)
{
	unsigned char src0, src1, src2, src3, src4, src5, src6,
		dst0, dst1, dst2, dst3, dst4, dst5, dst6, dst7;

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	/*
	  Do not write if all data in the block is not read yet,
	  or if read marker was not yet propagated to this core.
	*/
	if ((1 & (dst[0] | dst[1] | dst[2] | dst[3]
		  | dst[4] | dst[5] | dst[6] | dst[7])) != 0)
		return -1;

	/* Copy everything to the local variables */

	switch (size) {
	case 0:
		src0 = 0;
		/* Falls through */
	case 1:
		src1 = 0;
		/* Falls through */
	case 2:
		src2 = 0;
		/* Falls through */
	case 3:
		src3 = 0;
		/* Falls through */
	case 4:
		src4 = 0;
		/* Falls through */
	case 5:
		src5 = 0;
		/* Falls through */
	case 6:
		src6 = 0;
		/* Falls through */
	default:
		break;
	}

	switch (size) {
	case 7:
		src6 = src[6];
		/* Falls through */
	case 6:
		src5 = src[5];
		/* Falls through */
	case 5:
		src4 = src[4];
		/* Falls through */
	case 4:
		src3 = src[3];
		/* Falls through */
	case 3:
		src2 = src[2];
		/* Falls through */
	case 2:
		src1 = src[1];
		/* Falls through */
	case 1:
		src0 = src[0];
		/* Falls through */
	default:
		break;
	}

	/* Encode */
	dst0 = (src0 << 1) | 1;
	dst1 = ((src0 & 0x80 ) >> 6) | (src1 << 2)  | 1;
	dst2 = ((src1 & 0xc0 ) >> 5) | (src2 << 3)  | 1;
	dst3 = ((src2 & 0xe0 ) >> 4) | (src3 << 4)  | 1;
	dst4 = ((src3 & 0xf0 ) >> 3) | (src4 << 5)  | 1;
	dst5 = ((src4 & 0xf8 ) >> 2) | (src5 << 6)  | 1;
	dst6 = ((src5 & 0xfc ) >> 1) | (src6 << 7)  | 1;
	dst7 = src6 | 1;

	/* Write */
	dst[0] = dst0;
	dst[1] = dst1;
	dst[2] = dst2;
	dst[3] = dst3;
	dst[4] = dst4;
	dst[5] = dst5;
	dst[6] = dst6;
	dst[7] = dst7;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return 0;
}

/*
 * Write SEVEN bytes encoded as EIGHT bytes.
 *
 * Nonzero return value means that write area is not ready.
 */
static int write_encode_bytes_with_header(unsigned char volatile *dst,
					  const unsigned char *src,
					  unsigned int size,
					  char t, uint32_t msize)
{
	unsigned char src0, src1, src2, src3, src4, src5, src6,
		dst0, dst1, dst2, dst3, dst4, dst5, dst6, dst7;

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	/*
	  Do not write if all data in the block is not read yet,
	  or if read marker was not yet propagated to this core.
	*/
	if ((1 & (dst[0] | dst[1] | dst[2] | dst[3]
		  | dst[4] | dst[5] | dst[6] | dst[7])) != 0)
		return -1;

	/* Copy everything to the local variables */

	switch (size) {
	case 0:
		src5 = 0;
		/* Falls through */
	case 1:
		src6 = 0;
		/* Falls through */
	default:
		break;
	}

	switch (size) {
	case 2:
		src6 = src[1];
		/* Falls through */
	case 1:
		src5 = src[0];
		/* Falls through */
	default:
		src4 = (unsigned char)((msize >> 24) & 0xff);
		src3 = (unsigned char)((msize >> 16) & 0xff);
		src2 = (unsigned char)((msize >> 8) & 0xff);
		src1 = (unsigned char)(msize & 0xff);
		src0 = (unsigned char)t;
		break;
	}

	/* Encode */
	dst0 = (src0 << 1) | 1;
	dst1 = ((src0 & 0x80 ) >> 6) | (src1 << 2)  | 1;
	dst2 = ((src1 & 0xc0 ) >> 5) | (src2 << 3)  | 1;
	dst3 = ((src2 & 0xe0 ) >> 4) | (src3 << 4)  | 1;
	dst4 = ((src3 & 0xf0 ) >> 3) | (src4 << 5)  | 1;
	dst5 = ((src4 & 0xf8 ) >> 2) | (src5 << 6)  | 1;
	dst6 = ((src5 & 0xfc ) >> 1) | (src6 << 7)  | 1;
	dst7 = src6 | 1;

	/* Write */
	dst[0] = dst0;
	dst[1] = dst1;
	dst[2] = dst2;
	dst[3] = dst3;
	dst[4] = dst4;
	dst[5] = dst5;
	dst[6] = dst6;
	dst[7] = dst7;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return 0;
}

/*
 * Read SEVEN bytes encoded as EIGHT bytes.
 *
 * Nonzero return value means that data is not available.
 */
static int read_decode_bytes(unsigned char *dst,
			     unsigned char volatile *src,
			     unsigned int size)
{
	unsigned char src0, src1, src2, src3, src4, src5, src6, src7;

	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* Read all values */
	src0 = src[0];
	src1 = src[1];
	src2 = src[2];
	src3 = src[3];
	src4 = src[4];
	src5 = src[5];
	src6 = src[6];
	src7 = src[7];

	/*
	  Do not read if the data is not marked as written, or
	  markers are not yet propagated to this core.
	*/
	if ((1 & src0 & src1 & src2 & src3
	     & src4 & src5 & src6 & src7) != 1)
		return -1;

	/* Decode */
	switch (size) {
	case 7:
		dst[6] = src6 >> 7 | (src7 & 0xfe);
		/* Falls through */
	case 6:
		dst[5] = src5 >> 6 | ((src6 << 1) & 0xfc);
		/* Falls through */
	case 5:
		dst[4] = src4 >> 5 | ((src5 << 2) & 0xf8);
		/* Falls through */
	case 4:
		dst[3] = src3 >> 4 | ((src4 << 3) & 0xf0);
		/* Falls through */
	case 3:
		dst[2] = src2 >> 3 | ((src3 << 4) & 0xe0);
		/* Falls through */
	case 2:
		dst[1] = src1 >> 2 | ((src2 << 5) & 0xc0);
		/* Falls through */
	case 1:
		dst[0] = src0 >> 1 | ((src1 << 6) & 0x80);
		/* Falls through */
	default:
		break;
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return 0;
}

/*
 * Read SEVEN bytes encoded as EIGHT bytes.
 *
 * Nonzero return value means that data is not available.
 */
static int read_decode_bytes_with_header(unsigned char *dst,
					 unsigned char volatile *src,
					 unsigned int size,
					 char *type,
					 uint32_t *msize)
{
	unsigned char src0, src1, src2, src3, src4, src5, src6, src7;

	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* Read all values */
	src0 = src[0];
	src1 = src[1];
	src2 = src[2];
	src3 = src[3];
	src4 = src[4];
	src5 = src[5];
	src6 = src[6];
	src7 = src[7];

	/*
	  Do not read if the data is not marked as written, or
	  markers are not yet propagated to this core.
	*/
	if ((1 & src0 & src1 & src2 & src3
	     & src4 & src5 & src6 & src7) != 1)
		return -1;

	/* Decode */
	switch (size) {
	case 2:
		dst[1] = src6 >> 7 | (src7 & 0xfe);
		/* Falls through */
	case 1:
		dst[0] = src5 >> 6 | ((src6 << 1) & 0xfc);
		/* Falls through */
	default:
		*msize =
			(uint32_t)((src4 >> 5 | ((src5 << 2) & 0xf8)) << 24)
			| (uint32_t)((src3 >> 4 | ((src4 << 3) & 0xf0)) << 16)
			| (uint32_t)((src2 >> 3 | ((src3 << 4) & 0xe0)) << 8)
			| (uint32_t)(src1 >> 2 | ((src2 << 5) & 0xc0));
		*type = (char)(src0 >> 1 | ((src1 << 6) & 0x80));
		break;
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return 0;
}

/*
 * Create a request in a given area.
 */
int memipc_add_req(struct memipc_area *area, enum memipc_req_type req_type,
		   ssize_t req_size, unsigned char *req_data)
{
	size_t total_req_size, blocks_count, remainder,
		blocks_available, blocks_available_1, blocks_available_2,
		blocks_write, srcsize;
	unsigned i;
	unsigned char volatile *localptr, *endptr, *next_wptr;
	unsigned char *srcptr;
	unsigned inbuffer;

	/* Are we supposed to write here? */
	if (area->writer != memipc_my_pid) {
		fprintf(stderr, "Process %lu attempted to write a request "
			"to memipc area writable by a process %lu\n",
			(unsigned long)memipc_my_pid,
			(unsigned long)area->writer);
		return -1;
	}
	localptr = area->rptr;
	endptr = area->area + area->size;
	inbuffer = area->inbuffer;
	while ((((*localptr) & 1) == 0) && (inbuffer > 0)) {
		localptr ++;
		if (localptr >= endptr)
			localptr = area->area;
		inbuffer --;
	}
	area->rptr = localptr;
	area->inbuffer = inbuffer;

	/* Check if the area is full */
	if (area->inbuffer == area->size)
		return -1;

	/*
	  Determine the amount of data to be written. We use SEVEN-byte
	  blocks, each consuming EIGHT bytes in the buffer.
	*/
	total_req_size = req_size + sizeof(struct memipc_req_header);
	blocks_count = total_req_size / SEVEN;
	remainder = total_req_size - (blocks_count * SEVEN);
	if (remainder)
		blocks_count++;

	if (area->wptr < area->rptr) {
		blocks_available_1 = (area->rptr - area->wptr) / EIGHT;
		blocks_available_2 = 0;
		blocks_available = blocks_available_1;
	} else {
		blocks_available_1 = (endptr - area->wptr) / EIGHT;
		blocks_available_2 = (area->rptr - area->area) / EIGHT;
		blocks_available = blocks_available_1 + blocks_available_2;
	}

	/* Check if there is enough space to write the request */
	if (blocks_count > blocks_available)
		return -1;

	/* If there is a wrap around, start from writing wrapped data */
	if (blocks_count > blocks_available_1) {
		next_wptr = area->area
			+ (blocks_count - blocks_available_1) * EIGHT;
		for (i = blocks_count - blocks_available_1; i > 0; i --) {
			srcptr = req_data + blocks_available_1 * SEVEN
				+ i * SEVEN - sizeof(struct memipc_req_header);
			if ((srcptr - req_data) >= SEVEN) {
				srcsize = req_size
					- (srcptr - SEVEN - req_data);
				if (srcsize > SEVEN)
					srcsize = SEVEN;
				if (write_encode_bytes(area->area
						       + ((i - 1) * EIGHT),
						       srcptr - SEVEN, srcsize))
					return -1;
			} else {
				if (srcptr - req_data > req_size)
					srcptr = req_data + req_size;
				srcsize = srcptr - req_data;
				if (write_encode_bytes_with_header(
				area->area + ((i - 1) * EIGHT),
				req_data, srcsize,
				req_type,
				req_size + sizeof(struct memipc_req_header)))
					return -1;
			}
		}
		blocks_write = blocks_available_1;
	} else {
		/* No wrap around */
		blocks_write = blocks_count;
		next_wptr = area->wptr + (blocks_write) * EIGHT;
		if (next_wptr >= endptr)
			next_wptr = area->area;
	}

	/* Write data that was not wrapped around */
	for (i = blocks_write; i > 0 ; i --) {
		srcptr = req_data
			+ i * SEVEN - sizeof(struct memipc_req_header);
		if ((srcptr - req_data) >= SEVEN) {
			srcsize = req_size - (srcptr - SEVEN - req_data);
			if (srcsize > SEVEN)
				srcsize = SEVEN;
			if (write_encode_bytes(area->wptr + ((i - 1) * EIGHT),
					       srcptr - SEVEN, srcsize))
				return -1;
		} else {
			if (srcptr - req_data > req_size)
				srcptr = req_data + req_size;
			srcsize = srcptr - req_data;
			if (write_encode_bytes_with_header(
			area->wptr + ((i - 1) * EIGHT),
			req_data, srcsize,
			req_type,
			req_size + sizeof(struct memipc_req_header)))
				return -1;
		}

	}
	area->inbuffer += blocks_count * EIGHT;
	area->wptr = next_wptr;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return 0;
}

/*
 * Clear memory.
 *
 * memset() can not be used here because it may write multiple times
 * to the same location.
 */
static inline void memipc_clearmem(volatile unsigned char *p, size_t size)
{
	while (size-- > 0) *p++ = 0;
}

static inline int memipc_check_newdata(void)
{
	return (*memipc_check_newdata_ptr) & 1;
}

static inline struct memipc_area *get_s_memipc_mosi(struct
						    memipc_thread_params
						    *arg);
/*
 * Get request from a given area.
 */
int memipc_get_req(struct memipc_area *area, enum memipc_req_type *req_type,
		   ssize_t *req_size, unsigned char *req_data)
{
	size_t total_req_size, blocks_count, remainder,
		blocks_count_1, blocks_count_2, dstsize;
	unsigned i;
	unsigned char volatile *localptr, *endptr, *curr_rptr;
	unsigned char *dstptr;
	unsigned inbuffer;

	/* Are we supposed to read here? */
	if (area->reader != memipc_my_pid) {
		fprintf(stderr, "Process %lu attempted to read a request from "
			"memipc area readable by a process %lu\n",
			(unsigned long)memipc_my_pid,
			(unsigned long)area->reader);
		return -1;
	}

	localptr = area->wptr;
	endptr = area->area + area->size;

	/* Determine the amount of data in the buffer */
	inbuffer = area->inbuffer;
	while ((((*localptr) & 1) == 1) && (inbuffer < area->size)) {
		localptr ++;
		if (localptr >= endptr)
			localptr = area->area;
		inbuffer ++;
	}
	area->wptr = localptr;
	area->inbuffer = inbuffer;

	/* Check if the area has no complete blocks */
	if (area->inbuffer < EIGHT)
		return -1;

	/* Read the first header */
	uint32_t l_req_size;
	char l_req_type;
	if (read_decode_bytes_with_header(req_data, area->rptr,
					  SEVEN
					  - sizeof(struct memipc_req_header),
					  &l_req_type, &l_req_size))
		return -1;

	total_req_size = l_req_size;

	/* Check if there is sufficient amount of space in the destination */
	if (total_req_size > (*req_size + sizeof(struct memipc_req_header)))
		return -2;

	*req_size = total_req_size - sizeof(struct memipc_req_header);

	*req_type = l_req_type;

	if (total_req_size <= SEVEN) {
		/* Just one block, already written */

		/* Mark data as read */
		memipc_clearmem(area->rptr, EIGHT);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		area->rptr += EIGHT;
		if (area->rptr >= endptr)
			area->rptr = area->area;
		area->inbuffer -= EIGHT;
		/* Finished */
	} else {
		/* Multiple blocks */
		blocks_count = total_req_size / SEVEN;
		remainder = total_req_size - blocks_count * SEVEN;
		if (remainder)
			blocks_count++;

		/* Check if there is sufficient amount of data in the buffer */
		if (area->inbuffer < (blocks_count * EIGHT))
			return -1;

		blocks_count_1 = (endptr - area->rptr) / EIGHT;
		if (blocks_count <= blocks_count_1) {
			blocks_count_1 = blocks_count;
			blocks_count_2 = 0;
		} else {
			blocks_count_2 = blocks_count - blocks_count_1;
		}

		/* First block already copied */

		dstptr = req_data + SEVEN - sizeof(struct memipc_req_header);

		/* Read remaining data that was not wrapped around, if any */
		curr_rptr = area->rptr;
		for (i = 1; i < blocks_count_1; i++) {
			dstsize = total_req_size - i * SEVEN;
			if (dstsize > SEVEN)
				dstsize = SEVEN;
			if (read_decode_bytes(dstptr, curr_rptr + i * EIGHT,
					      dstsize))
				return -1;
			dstptr += dstsize;

		}
		curr_rptr += blocks_count_1 * EIGHT;

		/* If there is a wrap around, read wrapped around data */
		if (curr_rptr >= endptr) {
			curr_rptr = area->area;
			if (blocks_count_2 > 0) {
				for (i = 0; i < blocks_count_2; i++) {
					dstsize = total_req_size
						- (blocks_count_1 + i) * SEVEN;
					if (dstsize > SEVEN)
						dstsize = SEVEN;
					if (read_decode_bytes(dstptr,
							curr_rptr + i * EIGHT,
							dstsize))
						return -1;
					dstptr += dstsize;
				}
				curr_rptr += blocks_count_2 * EIGHT;
				memipc_clearmem(area->area,
						blocks_count_2 * EIGHT);
			}
		}
		memipc_clearmem(area->rptr, blocks_count_1 * EIGHT);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		area->rptr = curr_rptr;
		area->inbuffer -= blocks_count * EIGHT;
		/* Finished */
	}
	/* If we are reading from the thread input area, update the pointer */
	if ((memipc_thread_self != NULL)
	    && (area == get_s_memipc_mosi(memipc_thread_self)))
		memipc_check_newdata_ptr = area->rptr;
	return 0;
}

/*
 * Enter isolation mode.
 *
 * This should be only called internally, from the request handler.
 */
static int start_isolation(int cpu)
{
	cpu_set_t set;

	/* Exit from isolation, if still in isolation mode */
	prctl(PR_SET_TASK_ISOLATION, 0, 0, 0, 0);

	if (mlockall(MCL_CURRENT))
		return -1;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(cpu_set_t), &set))
		return -1;

	return prctl(PR_SET_TASK_ISOLATION,
		     PR_TASK_ISOLATION_ENABLE
		     | PR_TASK_ISOLATION_USERSIG
		     | PR_TASK_ISOLATION_SET_SIG(SIGUSR1), 0, 0, 0);
}

/* CPU sets for isolation */
static cpu_set_t _global_nonisol_cpuset, _global_isol_cpuset,
	_global_running_cpuset;
#if USE_CPU_SUBSETS
static char *server_socket_name = NULL;
#endif

/*
 * Exit from isolation mode.
 *
 * This should be only called internally, from the request handler.
 */
static void exit_isolation(void)
{
	/* Exit from isolation */
	prctl(PR_SET_TASK_ISOLATION, 0, 0, 0, 0);

	/*
	  The following, plus the mechanism that pushes processes away
	  from cores used for isolation, causes CPU to [eventually] become
	  idle, so scheduling timer will not be restarted. This seems to
	  be the only way to stop the scheduler timer once it started on
	  an isolated core despite the lack of other processes and threads
	  to schedule there.
	*/
	sched_setaffinity(0, sizeof(cpu_set_t), &_global_nonisol_cpuset);
}

/*
 * Text parser internals
 */

/* Skip whitespace. */
static void skip_whitespace(const char **p)
{
	while ((**p != 0) && ((unsigned const char)(**p) <= ' '))
		(*p)++;
}

/* Find the end of a non-whitespace token. */
static const char *find_endtoken(const char *p)
{
	while ((*p != 0) && ((unsigned const char)(*p) > ' '))
		p++;
	return p;
}

/* Skip a particular word. */
static int skip_word(const char **p, const char *word)
{
	const char *end, *curr;
	curr = *p;
	skip_whitespace(&curr);
	end = find_endtoken(curr);
	if (((end - curr) == (ssize_t) strlen(word))
	    && !strncmp(curr, word, end - curr)) {
		*p = end;
		skip_whitespace(p);
		return 0;
	} else
		return -1;
}

#if USE_CPU_SUBSETS
/* Skip whitespace. */
static void skip_whitespace_nconst(char **p)
{
	while ((**p != 0) && ((unsigned char)(**p) <= ' '))
		(*p)++;
}

/* Find the end of a non-whitespace token. */
static char *find_endtoken_nconst(char *p)
{
	while ((*p != 0) && ((unsigned const char)(*p) > ' '))
		p++;
	return p;
}
#endif

/* Get value from a hex digit. */
static unsigned char unhex(char c)
{
	if ((c >= '0') && (c <= '9'))
		return (unsigned char)c - '0';
	if ((c >= 'a') && (c <= 'f'))
		return (unsigned char)c - 'a' + 10;
	if ((c >='A') && (c <= 'F'))
		return (unsigned char)c - 'A' + 10;
	return 0;
}

/* Check if a string contains only decimal digits. */
static int is_all_decimal(const char *s)
{
	while (*s) {
		if ((*s < '0') || (*s > '9'))
			return 0;
		s++;
	}
	return 1;
}

/* Timers */

/*
  The header of /proc/timer_list contains the following line:

  --- 8< ---
  now at 80521821118000 nsecs
  --- >8 ---

  The value is shown as unsigned long long.  This shows the current
  time used for all other references.

  Here we are interested in running timers, not clocks. This means, we
  can, for now, safely ignore all clock-related information in
  /proc/timer_list , and limit ourselves to the timers themselves.

  After the header, first CPUs are listed with their timers. CPUs have
  headers "cpu:", then CPU number. After the header for each clock,
  staring with "clock %d:" with the clock number, "active timers:"
  line precedes all high resolution timers for a given clock, "active
  jiffie timers:" was proposed to precede low resolution timers list,
  but currently is not used.

  Timers can be represented as:

  --- 8< ---
  #0: <ffff8003fda67bd0>, tick_sched_timer, S:01
  # expires at 78753860000000-78753860000000 nsecs [in 3758380 to 3758380 nsecs]
  --- >8 ---

  The value after "S:" contains state:

  #define HRTIMER_STATE_INACTIVE	0x00
  #define HRTIMER_STATE_ENQUEUED	0x01

  After the end of this list for each CPU there is:

  --- 8< ---
  .expires_next   : 78753860000000 nsecs
  .hres_active    : 1
  .nr_events      : 19689092
  .nr_retries     : 0
  .nr_hangs       : 0
  .max_hang_time  : 0
  .nohz_mode      : 2
  .last_tick      : 0 nsecs
  .tick_stopped   : 0
  .idle_jiffies   : 0
  .idle_calls     : 0
  .idle_sleeps    : 0
  .idle_entrytime : 78753854647850 nsecs
  .idle_waketime  : 0 nsecs
  .idle_exittime  : 0 nsecs
  .idle_sleeptime : 31370976730450 nsecs
  .iowait_sleeptime: 0 nsecs
  .last_jiffies   : 0
  .next_timer     : 0
  .idle_expires   : 0 nsecs
  jiffies: 4314580761

  --- >8 ---

  After all CPUs there is a list of tick devices.

  Tick devices are described after "Tick Device: mode:", either as
  "Broadcast device" or "Per CPU device:".
  "Broadcast device" has "tick_broadcast_mask:" and
  "tick_broadcast_oneshot_mask:" attributes, containing a map of CPUs.
  "Per CPU device: " has CPU number.

  --- 8< ---
  Tick Device: mode:     1
  Broadcast device
  Clock Event Device: bc_hrtimer
  max_delta_ns:   9223372036854775807
  min_delta_ns:   1
  mult:           1
  shift:          0
  mode:           1
  next_event:     9223372036854775807 nsecs
  set_next_event: <0000000000000000>
  shutdown: bc_shutdown
  event_handler:  tick_handle_oneshot_broadcast
  retries:        0

  tick_broadcast_mask: 000000
  tick_broadcast_oneshot_mask: 000000

  --- >8 ---

  --- >8 ---
  Tick Device: mode:     1
  Per CPU device: 0
  Clock Event Device: arch_sys_timer
  max_delta_ns:   21474836451
  min_delta_ns:   1000
  mult:           429496730
  shift:          32
  mode:           3
  next_event:     79072484000000 nsecs
  set_next_event: arch_timer_set_next_event_phys
  shutdown: arch_timer_shutdown_phys
  oneshot stopped: arch_timer_shutdown_phys
  event_handler:  hrtimer_interrupt
  retries:        0

  --- >8 ---

  There is a value KTIME_MAX equal to ((s64)~((u64)1 << 63)), that indicates
  never expiring timer. It is printed as "9223372036854775807".
*/

/* High resolution timer state bits, from include/linux/hrtimer.h */
#define HRTIMER_STATE_INACTIVE	0x00
#define HRTIMER_STATE_ENQUEUED	0x01

/*
  "Tick Device: mode:" value in tick device header,
  from kernel/time/tick-sched.h
*/
enum tick_device_mode {
	TICKDEV_MODE_PERIODIC,
	TICKDEV_MODE_ONESHOT,
};

/*
  "mode:" for tick devices, from include/linux/clockchips.h

  We are interested in CLOCK_EVT_STATE_PERIODIC and CLOCK_EVT_STATE_ONESHOT.
*/
enum clock_event_state {
	CLOCK_EVT_STATE_DETACHED,
	CLOCK_EVT_STATE_SHUTDOWN,
	CLOCK_EVT_STATE_PERIODIC,
	CLOCK_EVT_STATE_ONESHOT,
	CLOCK_EVT_STATE_ONESHOT_STOPPED,
};

/*
  KTIME_MAX from include/linux/time.h
*/
#define KTIME_MAX			((int64_t)~((uint64_t)1 << 63))

enum isol_timer_type {
    ISOL_TIMER_HRTIMER,
    ISOL_TIMER_CPUTIMER,
    ISOL_TIMER_BTICKDEV,
    ISOL_TIMER_CPUTICKDEV
};

#if DEBUG_ISOL_VERBOSE
static const char *isol_timer_type_name[] = {
	"HR timer",
	"CPU timer",
	"Tick",
	"Tick (CPU)"
};
#endif

/*
  Timer list for debugging purposes.
 */
struct isol_linux_timer {
#if DEBUG_ISOL_NAMES
	char *addr;
	char *handler;
#endif
	enum isol_timer_type timer_type;
	int64_t last_updated;
	int64_t expires;
	struct isol_linux_timer *next;
};

/*
 * Parse the first line of a high-resolution timer description.
 */
static int hrtimer_parse_line_1(const char *s,
				int *count,
#if DEBUG_ISOL_NAMES
				char **addr, char **name,
#endif
				int *state)
{
	const char *p, *e;
#if DEBUG_ISOL_NAMES
	char *a_addr, *a_name;
#endif
	int a_count, a_state;

	p = s;
	skip_whitespace(&p);
	e = strchr(p, ':');
	if (e == NULL)
		return -1;
	if (sscanf(p, "%d", &a_count) != 1)
		return -1;
	p = e + 1;
	skip_whitespace(&p);
	e = strchr(p, ',');
	if (e == NULL)
		return -1;
#if DEBUG_ISOL_NAMES
	a_addr = malloc(e - p + 1);
	if (a_addr == NULL)
		return -1;
	memcpy(a_addr, p, e - p);
	a_addr[e - p] = 0;
#endif
	p = e + 1;
	skip_whitespace(&p);
	e = strchr(p, ',');
	if (e == NULL) {
#if DEBUG_ISOL_NAMES
		free(a_addr);
#endif
		return -1;
	}
#if DEBUG_ISOL_NAMES
	a_name = malloc(e - p + 1);
	if (a_name == NULL) {
		free(a_addr);
		return -1;
	}
	memcpy(a_name, p, e - p);
	a_name[e - p] = 0;
#endif
	p = e + 1;
	skip_whitespace(&p);
	if ((p[0] != 'S') || (p[1] != ':')
	    || sscanf(p + 2, "%d", &a_state) != 1)
		a_state = 1;
	*count = a_count;
#if DEBUG_ISOL_NAMES
	*addr = a_addr;
	*name = a_name;
#endif
	*state = a_state;
	return 0;
}

/*
 * Parse the second line of a high-resolution timer description.
 */
static int hrtimer_parse_line_2(const char *s, int64_t *softexp, int64_t *exp)
{
	const char *p, *e;
	uint64_t a_softexp, a_exp;
	p = s;
	if (skip_word(&p, "expires"))
		return -1;
	if (skip_word(&p, "at"))
		return -1;
	e = strchr(p, '-');
	if (e == NULL)
		return -1;
	if ((sscanf(p, "%lu", &a_softexp) != 1)
	    || (sscanf(e + 1, "%lu", &a_exp) != 1))
		return -1;
	*softexp = a_softexp;
	*exp = a_exp;
	return 0;
}

/*
 * Get CPU set from its hex representation.
 */
static int get_cpuset(const char *s, cpu_set_t *cpuset)
{
	const char *start, *p;
	unsigned char val;
	int i, n, cpus_in_set, count;
	start = s;
	skip_whitespace(&start);
	p = start;
	while ((*p) &&
	       (((*p >= '0') && (*p <= '9'))
		|| ((*p >='A') && (*p <= 'F'))
		|| ((*p >= 'a') && (*p <= 'f'))))
		p++;
	n = (p - start) * 4;
	cpus_in_set = (n > CPU_SETSIZE)?CPU_SETSIZE:n;

	CPU_ZERO(cpuset);
	for (i = 0, count = 0; i < (p - start); i++) {
		val = unhex(*(p - i - 1));
		if ((val & 1) && ((i * 4) < cpus_in_set)) {
			CPU_SET((i * 4), cpuset);
			count++;
		}
		if ((val & 2) && ((i * 4 + 1) < cpus_in_set)) {
			CPU_SET((i * 4 + 1), cpuset);
			count++;
		}
		if ((val & 4) && ((i * 4 + 2) < cpus_in_set)) {
			CPU_SET((i * 4 + 2), cpuset);
			count++;
		}
		if ((val & 8) && ((i * 4 + 3) < cpus_in_set)) {
			CPU_SET((i * 4 + 3), cpuset);
			count++;
		}
	}
	return count;
}

#if DEBUG_ISOL_VERBOSE
/*
 * Print CPU set as a human-readable list.
 */
static void print_cpuset(FILE *f, cpu_set_t *cpuset)
{
	int i, flag;
	for (i = 0, flag = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, cpuset)) {
			if (flag)
				fprintf(f, ", %d", i);
			else
				fprintf(f, "%d", i);
			flag = 1;
		}
	}
	if (flag == 0)
		fprintf(f, "<empty>");
}
#endif

#define TICK_DEV_KNOWN_NONE		(0x00)
#define TICK_DEV_KNOWN_CPU		(0x01)
#define TICK_DEV_KNOWN_STATE		(0x02)
#define TICK_DEV_KNOWN_NEXT_EVENT	(0x04)
#define TICK_DEV_KNOWN_BCAST_SET	(0x08)
#define TICK_DEV_KNOWN_BCAST_OS_SET	(0x10)

static int cpu_update_timer(enum isol_timer_type timer_type,
#if DEBUG_ISOL_NAMES
			    const char *addr, const char *handler,
#endif
			    int cpu, int64_t expire, int64_t now);

static void cpu_remove_expired_timers(int64_t now);

static int memipc_add_timer_to_desc(int cpu,
#if DEBUG_ISOL_NAMES
				    const char *addr,
				    const char *handler,
#endif
				    enum isol_timer_type timer_type,
				    int64_t last_updated,
				    int64_t expires);
#if 0
static void memipc_remove_timers_from_desc(int cpu);
#endif

static void memipc_remove_timers_from_all_desc(void);

#if DEBUG_ISOL_VERBOSE
static void show_all_timers_by_desc(FILE *f, cpu_set_t *cpuset);
#endif

static int64_t remaining_nsec_before_expiration(int64_t now);

/*
 * Process all timers visible in /proc/timer_list , determine timers
 * still running on CPUs that are intended for isolation.  cpuset is
 * set to all CPUs intended for isolation, that have timers running on
 * them.
 */
static int process_all_timers(cpu_set_t *cpuset, int64_t *now)
{
	static const char *token_timers_list[] = {
	    "now",
	    "cpu:",
	    "active",
	    ".expires_next",
	    "Tick",
	    "Broadcast",
	    "Per",
	    "mode:",
	    "next_event:",
	    "tick_broadcast_mask:",
	    "tick_broadcast_oneshot_mask:"
	};

	static int token_timers_list_len[] = {
	    3,
	    4,
	    6,
	    13,
	    4,
	    9,
	    3,
	    5,
	    11,
	    20,
	    28
	};

	char stringbuf[1024];
	const char *p, *e;
	FILE *f;
	int i, retval = 0;
	enum {
	      T_PARS_START,
	      T_PARS_CPU_LIST,
	      T_PARS_CPU,
	      T_PARS_ACT,
	      T_PARS_ACT_PROCESSING,
	      T_PARS_TDEV,
	      T_PARS_TDEV_BCAST,
	      T_PARS_TDEV_CPU
	} parser_state = T_PARS_START;

	enum {
	      T_TOKEN_NOW,
	      T_TOKEN_CPU,
	      T_TOKEN_ACTIVE,
	      T_TOKEN_EXPIRES_NEXT,
	      T_TOKEN_TICK,
	      T_TOKEN_BCAST,
	      T_TOKEN_PER,
	      T_TOKEN_MODE,
	      T_TOKEN_NEXT_EVENT,
	      T_TOKEN_BCAST_MASK,
	      T_TOKEN_BCAST_ONESHOT_MASK,
	      T_TOKEN_NONE
	} token = T_TOKEN_NONE;

	f = fopen("/proc/timer_list", "rt");
	if (f == NULL)
		return -1;
	parser_state = T_PARS_START;
	int64_t now_at = KTIME_MAX, expires_next = KTIME_MAX,
		hrtimer_softexp = KTIME_MAX, hrtimer_exp = KTIME_MAX,
		tick_dev_next_event = KTIME_MAX;
	int curr_cpu = -1, tick_dev_cpu = -1;
	int hrtimer_parse_err = 0;
	int hrtimer_count = 0;
#if DEBUG_ISOL_NAMES
	char *hrtimer_func = NULL;
	char *hrtimer_addr = NULL;
#endif
	int hrtimer_state = 0;
	int tick_dev_mode = 0, tick_dev_state = 0,
		tick_dev_known = TICK_DEV_KNOWN_NONE;
	cpu_set_t tick_dev_cpuset, tick_dev_os_cpuset;
	CPU_ZERO(&tick_dev_cpuset);
	CPU_ZERO(&tick_dev_os_cpuset);
	CPU_ZERO(cpuset);

	memipc_remove_timers_from_all_desc();

	while (fgets(stringbuf, sizeof(stringbuf), f) != NULL) {
		p = stringbuf;
		skip_whitespace(&p);
		e = find_endtoken(p);
		if (e != p) {
			if (*p == '#') {
				switch (parser_state) {
				case T_PARS_ACT:
					parser_state = T_PARS_ACT_PROCESSING;
#if DEBUG_ISOL_NAMES
					if (hrtimer_func) {
						free(hrtimer_func);
						hrtimer_func = NULL;
					}
					if (hrtimer_addr) {
						free(hrtimer_addr);
						hrtimer_addr = NULL;
					}
#endif
					hrtimer_parse_err =
						hrtimer_parse_line_1(p + 1,
								&hrtimer_count,
#if DEBUG_ISOL_NAMES
								&hrtimer_addr,
								&hrtimer_func,
#endif
								&hrtimer_state);
				    break;
				case T_PARS_ACT_PROCESSING:
					parser_state = T_PARS_ACT;
					hrtimer_parse_err |=
						hrtimer_parse_line_2(p + 1,
							&hrtimer_softexp,
							&hrtimer_exp);
					if (!hrtimer_parse_err) {
						if ((hrtimer_state
						     != HRTIMER_STATE_INACTIVE)
						    && ((hrtimer_exp
							 != KTIME_MAX)
							|| (hrtimer_softexp
							    != KTIME_MAX))
						    ) {
							if (cpu_update_timer(
						ISOL_TIMER_HRTIMER,
#if DEBUG_ISOL_NAMES
						hrtimer_addr,
						hrtimer_func,
#endif
						curr_cpu,
						hrtimer_exp,
						now_at))
						CPU_SET(curr_cpu,cpuset);

						memipc_add_timer_to_desc(
							curr_cpu,
#if DEBUG_ISOL_NAMES
							hrtimer_addr,
							hrtimer_func,
#endif
							ISOL_TIMER_HRTIMER,
							now_at, hrtimer_exp);
						}
					}
					break;
				default:
					break;
				}
			} else {
				for (i = 0, token = T_TOKEN_NONE;
				     (token == T_TOKEN_NONE)
					     && (i < T_TOKEN_NONE);
				    i++) {
					if (((e - p) ==
					     token_timers_list_len[i])
					    && !strncmp(p, token_timers_list[i],
							e - p))
						token = i;
				}
				switch (token) {
				case T_TOKEN_NOW:
					if ((parser_state == T_PARS_START)
					    && !skip_word(&e, "at")
					    && sscanf(e, "%lu",
						      (uint64_t*)
						      &now_at) == 1) {
						parser_state = T_PARS_CPU_LIST;
					}
					break;
				case T_TOKEN_CPU:
					if (((parser_state == T_PARS_CPU_LIST)
					     || (parser_state == T_PARS_CPU)
					     || (parser_state == T_PARS_ACT))
					    && sscanf(e, "%d", &curr_cpu)
					    == 1) {
						parser_state = T_PARS_CPU;
						expires_next = KTIME_MAX;
					}
					break;
				case T_TOKEN_ACTIVE:
					if (!skip_word(&e, "timers:")
					    && (parser_state == T_PARS_CPU)) {
						parser_state = T_PARS_ACT;
					}
					break;
				case T_TOKEN_EXPIRES_NEXT:
					if (((parser_state == T_PARS_CPU)
					     || (parser_state == T_PARS_ACT))
					    && !skip_word(&e, ":")
					    && sscanf(e, "%lu",
						      (uint64_t*)
						      &expires_next) == 1) {
						parser_state = T_PARS_CPU_LIST;
						if ((expires_next != KTIME_MAX)
						    ) {
						if (cpu_update_timer(
						    ISOL_TIMER_CPUTIMER,
#if DEBUG_ISOL_NAMES
						     "cpu_timer",
						     "none",
#endif
						     curr_cpu,
						     expires_next,
						     now_at))
							CPU_SET(curr_cpu,
								cpuset);
#if DEBUG_ISOL_LIST_ALL_TIMER_ENTRIES
						memipc_add_timer_to_desc(
							curr_cpu,
#if DEBUG_ISOL_NAMES
							"cpu_timer",
							"none",
#endif
							ISOL_TIMER_CPUTIMER,
							now_at, expires_next);
#endif
						}
					}
				    break;
				case T_TOKEN_TICK:
					if (((parser_state == T_PARS_CPU_LIST)
					 || (parser_state == T_PARS_CPU)
					 || (parser_state == T_PARS_ACT)
					 || (parser_state == T_PARS_TDEV)
					 || (parser_state == T_PARS_TDEV_BCAST)
					 || (parser_state == T_PARS_TDEV_CPU))
					    && !skip_word(&e, "Device:")
					    && !skip_word(&e, "mode:")
					    && sscanf(e, "%d", &tick_dev_mode)
					    == 1) {
						parser_state = T_PARS_TDEV;
					}
					break;
				case T_TOKEN_BCAST:
					if ((parser_state == T_PARS_TDEV)
					    && !skip_word(&e, "device")) {
						parser_state =
							T_PARS_TDEV_BCAST;
						tick_dev_known =
							TICK_DEV_KNOWN_NONE;
					}
					break;
				case T_TOKEN_PER:
					if ((parser_state == T_PARS_TDEV)
					    && !skip_word(&e, "CPU")
					    && !skip_word(&e, "device:")
					    && sscanf(e, "%d", &tick_dev_cpu)
					    == 1) {
						parser_state = T_PARS_TDEV_CPU;
						tick_dev_known =
							TICK_DEV_KNOWN_CPU;
					}
					break;
				case T_TOKEN_MODE:
					if (((parser_state == T_PARS_TDEV_BCAST)
					   || (parser_state == T_PARS_TDEV_CPU))
					    && sscanf(e, "%d",
						      &tick_dev_state) == 1) {
						tick_dev_known |=
							TICK_DEV_KNOWN_STATE;
					}
					break;
				case T_TOKEN_NEXT_EVENT:
					if (((parser_state == T_PARS_TDEV_BCAST)
					   || (parser_state == T_PARS_TDEV_CPU))
					    && sscanf(e, "%lu",
						      (uint64_t *)
						      &tick_dev_next_event)
					    == 1) {
						tick_dev_known |=
						TICK_DEV_KNOWN_NEXT_EVENT;
					}
					break;
				case T_TOKEN_BCAST_MASK:
					if (parser_state == T_PARS_TDEV_BCAST) {
						get_cpuset(e, &tick_dev_cpuset);
						tick_dev_known |=
						TICK_DEV_KNOWN_BCAST_SET;
					}
					break;
				case T_TOKEN_BCAST_ONESHOT_MASK:
					if (parser_state == T_PARS_TDEV_BCAST) {
						get_cpuset(e,
							   &tick_dev_os_cpuset);
						tick_dev_known |=
						TICK_DEV_KNOWN_BCAST_OS_SET;
					}
					break;
				default:
					break;
				}
			}
			switch (parser_state) {
			case T_PARS_TDEV_CPU:
				if ((tick_dev_known &
				     (TICK_DEV_KNOWN_CPU
				      | TICK_DEV_KNOWN_STATE
				      | TICK_DEV_KNOWN_NEXT_EVENT)) ==
				    (TICK_DEV_KNOWN_CPU
				     | TICK_DEV_KNOWN_STATE
				     | TICK_DEV_KNOWN_NEXT_EVENT)) {
					if (((tick_dev_state
					      == CLOCK_EVT_STATE_PERIODIC)
					     || (tick_dev_state
						 == CLOCK_EVT_STATE_ONESHOT))
					    && (tick_dev_next_event
						!= KTIME_MAX)
					    ) {
						if (cpu_update_timer(
							ISOL_TIMER_CPUTICKDEV,
#if DEBUG_ISOL_NAMES
							"tick_device",
							"none",
#endif
							tick_dev_cpu,
							tick_dev_next_event,
							now_at))
							CPU_SET(tick_dev_cpu,
								cpuset);
#if DEBUG_ISOL_LIST_ALL_TIMER_ENTRIES
						memipc_add_timer_to_desc(
							tick_dev_cpu,
#if DEBUG_ISOL_NAMES
							"tick_device",
							"none",
#endif
							ISOL_TIMER_CPUTICKDEV,
							now_at,
							tick_dev_next_event);
#endif
						tick_dev_known =
							TICK_DEV_KNOWN_NONE;
					}
				}
				break;
			case T_PARS_TDEV_BCAST:
				if ((tick_dev_known &
				     (TICK_DEV_KNOWN_STATE
				      | TICK_DEV_KNOWN_NEXT_EVENT
				      | TICK_DEV_KNOWN_BCAST_SET
				      | TICK_DEV_KNOWN_BCAST_OS_SET)) ==
				    (TICK_DEV_KNOWN_STATE
				     | TICK_DEV_KNOWN_NEXT_EVENT
				     | TICK_DEV_KNOWN_BCAST_SET
				     | TICK_DEV_KNOWN_BCAST_OS_SET)) {
					if (((tick_dev_state
					      == CLOCK_EVT_STATE_PERIODIC)
					     || (tick_dev_state
						 == CLOCK_EVT_STATE_ONESHOT))
					    && (tick_dev_next_event
						!= KTIME_MAX)
					    && ((CPU_COUNT(&tick_dev_cpuset)
						 != 0)
					      ||(CPU_COUNT(&tick_dev_os_cpuset)
						 != 0))) {
						int cpunum;
						for (cpunum = 0;
						     cpunum < CPU_SETSIZE;
						     cpunum ++)
							if (CPU_ISSET(cpunum,
							&tick_dev_cpuset)
							    || CPU_ISSET(cpunum,
							&tick_dev_os_cpuset))
							if (cpu_update_timer(
							ISOL_TIMER_BTICKDEV,
#if DEBUG_ISOL_NAMES
							"tick_dev",
							"none",
#endif
							cpunum,
							tick_dev_next_event,
							now_at))
								CPU_SET(cpunum,
									cpuset);
#if DEBUG_ISOL_LIST_ALL_TIMER_ENTRIES
						memipc_add_timer_to_desc(
							cpunum,
#if DEBUG_ISOL_NAMES
							"tick_dev",
							"none",
#endif
							ISOL_TIMER_BTICKDEV,
							now_at,
							tick_dev_next_event);
#endif
					}
					tick_dev_known =
						TICK_DEV_KNOWN_NONE;
				}
				break;
			default:
				break;
			}
		}
	}
	fclose(f);
#if DEBUG_ISOL_NAMES
	if (hrtimer_func) {
		free(hrtimer_func);
		hrtimer_func = NULL;
	}
	if (hrtimer_addr) {
		free(hrtimer_addr);
		hrtimer_addr = NULL;
	}
#endif
	if (now_at != KTIME_MAX) {
		*now = now_at;
		cpu_remove_expired_timers(now_at);
	}
	return retval;
}

/* Processes and threads */

/*
 * "Foreign thread" descriptor for process/thread table.
 */

/* struct memipc_thread_params; */

struct foreign_thread_desc {
	pid_t pid;
	pid_t tid;
	pthread_t thread;

	/* non-NULL only for managed threads */
	struct memipc_thread_params *isolated_thread;

#if DEBUG_ISOL_NAMES
	char *name;
#endif
	cpu_set_t cpus_allowed;
	int cpu;
	int vol_context_switches;
	int nonvol_context_switches;
	cpu_set_t prev_cpus_allowed;
	int prev_cpu;
	int prev_vol_context_switches;
	int prev_nonvol_context_switches;
	int update_flag;
};

/*
 * List of all threads visible on the system. This list is allocated
 * on first scan and gets re-allocated if it has to grow. It never
 * shrinks or freed.
 */
static struct foreign_thread_desc *proctable = NULL;
static int proctable_size = 0, proctable_alloc = 0;

/*
 * Block sizes for dynamic allocation.
 */
#define PROCTABLE_INIT_ALLOC_SIZE (10)
#define PROCTABLE_INC_ALLOC_SIZE (4)

static int memipc_attach_thread_to_desc(struct foreign_thread_desc *desc);
static void memipc_detach_thread_from_desc(struct foreign_thread_desc *desc);
static void memipc_update_foreign_thread(struct foreign_thread_desc *desc);

/*
 * Initialize the thread entry for the first time.
 */
static int proctable_init_thread(struct foreign_thread_desc *dst,
				 struct foreign_thread_desc *src)
{
	memset(dst, 0, sizeof(struct foreign_thread_desc));
#if DEBUG_ISOL_NAMES
	if (src->name != NULL)
		dst->name = strdup(src->name);
	else
		dst->name = strdup("(null)");
#endif
	dst->pid = src->pid;
	dst->tid = src->tid;
	dst->thread = src->thread;
	dst->cpus_allowed = src->cpus_allowed;
	dst->cpu = src->cpu;
	dst->vol_context_switches = src->vol_context_switches;
	dst->nonvol_context_switches = src->nonvol_context_switches;

	memipc_attach_thread_to_desc(dst);

	dst->update_flag = 1;

	return 0;
}

/*
 * Update the thread entry.
 */
static int proctable_update_thread(struct foreign_thread_desc *dst,
				   struct foreign_thread_desc *src)
{

#if DEBUG_ISOL_NAMES
	if ((src->name != NULL)
	    && (dst->name != NULL)
	    && strcmp(src->name, dst->name)) {
		int sl, dl;
		sl = strlen(src->name);
		dl = strlen(dst->name);
		if (dl < sl) {
			free(dst->name);
			dst->name = strdup(src->name);
		}
		else
			memcpy(dst->name, src->name, sl + 1);
	}
#endif

	dst->prev_cpus_allowed = dst->cpus_allowed;
	dst->cpus_allowed = src->cpus_allowed;

	dst->prev_cpu = dst->cpu;
	dst->cpu = src->cpu;

	dst->prev_vol_context_switches = dst->vol_context_switches;
	dst->vol_context_switches = src->vol_context_switches;

	dst->prev_nonvol_context_switches = dst->nonvol_context_switches;
	dst->nonvol_context_switches = src->nonvol_context_switches;

	memipc_attach_thread_to_desc(dst);

	dst->update_flag = 1;
	return 0;
}

/*
 * Add a new thread to the table or update existing one.
 */
static int proctable_add_thread(struct foreign_thread_desc *src)
{
	int i, j;
	for (i = 0; i < proctable_size ; i++) {
		if ((proctable[i].pid == src->pid)
		    && (proctable[i].tid == src->tid))
			return proctable_update_thread(&proctable[i], src);
	}
	if (proctable_size >= proctable_alloc) {
		if (proctable_alloc == 0) {
			proctable = (struct foreign_thread_desc *)
				malloc(sizeof(struct foreign_thread_desc)
				       * PROCTABLE_INIT_ALLOC_SIZE);
			if (proctable == NULL)
				return -1;
			proctable_size = 0;
			proctable_alloc = PROCTABLE_INIT_ALLOC_SIZE;
		} else {
			struct foreign_thread_desc *tmptable;
			tmptable = (struct foreign_thread_desc *)
				realloc(proctable,
					sizeof(struct foreign_thread_desc)
					* (proctable_alloc +
					   PROCTABLE_INC_ALLOC_SIZE));
			if (tmptable == NULL)
				return -1;
			proctable = tmptable;
			for (j = 0; j < proctable_size; j++) {
				if (proctable[j].isolated_thread != NULL)
					memipc_update_foreign_thread(
							&proctable[j]);
			}
			proctable_alloc += PROCTABLE_INC_ALLOC_SIZE;
		}
	}
	return proctable_init_thread(&proctable[proctable_size++], src);
}

/*
 * Remove all threads that were not updated.
 */
static void cleanup_threads(void)
{
	int i;
	struct foreign_thread_desc *dst, *src;

	for (src = proctable, dst = proctable, i = 0; i < proctable_size; i++) {
		if (src->update_flag == 1) {
			if (src != dst) {
				memcpy(dst, src,
				       sizeof(struct foreign_thread_desc));
				if (dst->isolated_thread != NULL)
					memipc_update_foreign_thread(dst);
			}
			dst->update_flag = 0;
			dst++;
		} else {
			if (src->isolated_thread != NULL)
				memipc_detach_thread_from_desc(src);
#if DEBUG_ISOL_NAMES
			if (src->name != NULL)
				free(src->name);
#endif
		}
		src++;
	}
	proctable_size = dst - proctable;
}

#if DEBUG_ISOL_VERBOSE
static const char *memipc_thread_state_name(struct memipc_thread_params *s);

/*
 * Show "foreign" thread.
 *
 * If it happens to be a managed thread, also show its state.
 */
static void show_thread_desc(struct foreign_thread_desc *desc)
{
#if DEBUG_ISOL_NAMES
	if (desc->name) {
		fprintf(stderr, "%c %u \"%s\", CPU %d, ctxt sw: %d/%d",
			(desc->pid == desc->tid)?'*':' ',
			desc->tid, desc->name,
			desc->cpu,
			desc->vol_context_switches,
			desc->nonvol_context_switches);
		if (desc->isolated_thread != NULL) {
			fprintf(stderr, " (state: %s)\n",
				memipc_thread_state_name(
						desc->isolated_thread));
		} else
		    fprintf(stderr, "\n");
	} else
#endif
		{
			fprintf(stderr, "%c %u \"?\", CPU %d, ctxt sw: %d/%d",
				(desc->pid == desc->tid)?'*':' ',
				desc->tid,
				desc->cpu,
				desc->vol_context_switches,
				desc->nonvol_context_switches);
			if (desc->isolated_thread != NULL) {
				fprintf(stderr, " (state: %s)\n",
					memipc_thread_state_name(
							desc->isolated_thread));
			} else
				fprintf(stderr, "\n");
		}
}
#endif

#if DEBUG_ISOL_VERBOSE
/* This is completely disabled if verbose debugging is not enabled. */
#define ISOL_PROC_LIST_CMD_SHOW		(0x01)
#endif
#define ISOL_PROC_LIST_CMD_PUSH_AWAY	(0x02)

/*
 * Process a single line from /proc/<pid>/task/<tid>/status file.
 */
static int update_proc_status(struct foreign_thread_desc *desc,
		       const char *s, unsigned pid, unsigned tid, int cmd)
{
	static const char *token_status[] = {
	    "Name:",
	    "Cpus_allowed:",
	    "voluntary_ctxt_switches:",
	    "nonvoluntary_ctxt_switches:"
	};

	static int token_status_len[] =	{
	    5,
	    13,
	    24,
	    27
	};

	enum {
	      S_TOKEN_NAME,
	      S_TOKEN_CPUS_ALLOWED,
	      S_TOKEN_VOL_CTXT_SW,
	      S_TOKEN_NONVOL_CTXT_SW,
	      S_TOKEN_NONE
	} token = S_TOKEN_NONE;

	const char *p, *e;
	int i;

	p = s;
	skip_whitespace(&p);
	e = find_endtoken(p);
	if (e != p) {
		for (i = 0, token = S_TOKEN_NONE;
		     (token == S_TOKEN_NONE) && (i < S_TOKEN_NONE);
		     i++) {
			if (((e - p)	== token_status_len[i])
			    && !strncmp(p, token_status[i],
					e - p)) {
				token = i;
				skip_whitespace(&e);
				p = e;
				e = strchr(p, '\n');
				if (e == NULL)
					e = p + strlen(p);
			}
		}
		switch (token) {
		case S_TOKEN_NAME:
#if DEBUG_ISOL_NAMES
			if (desc->name != NULL)
				free(desc->name);
			desc->name = malloc(e - p + 1);
			if (desc->name != NULL) {
				memcpy(desc->name, p, e - p);
				desc->name[e - p] = 0;
			}
#endif
			break;
		case S_TOKEN_CPUS_ALLOWED:
			get_cpuset(p, &desc->cpus_allowed);
			break;
		case S_TOKEN_VOL_CTXT_SW:
			sscanf(p, "%d", &desc->vol_context_switches);
			break;
		case S_TOKEN_NONVOL_CTXT_SW:
			sscanf(p, "%d", &desc->nonvol_context_switches);
			break;
		default:
			break;
		}
	}
	return 0;
}

/*
 * Process a /proc/<pid>/task/<tid>/stat file.
 */
static int get_proc_stat(struct foreign_thread_desc *desc,
			 const char *s, unsigned pid, unsigned tid, int cmd)
{
	const char *p, *e;
	int i, cpunum;
	/* Ignore fields 1 (pid) and 2 (comm), find the end of field 2 */
	p = strrchr(s, ')');
	if (p == NULL)
		return -1;
	p++;
	skip_whitespace(&p);
	/* Field 3 (state) */
	for (i = 3; i < 39; i++) {
		e = find_endtoken(p);
		if (e == p)
			return -1;
		skip_whitespace(&e);
		p = e;
	}
	e = find_endtoken(p);
	if (e == p)
		return -1;
	if (sscanf(p, "%d", &cpunum) != 1)
		return -1;
	desc->cpu = cpunum;
	return 0;
}

/*
 * Scan /proc and process all processes and their threads.
 */
static int process_all_threads(cpu_set_t *cpuset, int cmd)
{
	struct foreign_thread_desc currproc;

	char stringbuf[1024];
	struct dirent *procdirent, *taskdirent;
	DIR *processes, *tasks;
	FILE *sfile;
	unsigned int pid, tid;

	processes = opendir("/proc");
	if (processes == NULL)
		return -1;

	while ((procdirent = readdir(processes))) {
		if (is_all_decimal(procdirent->d_name)
		    && (sscanf(procdirent->d_name,
			       "%u", &pid) == 1)) {
			snprintf(stringbuf, sizeof(stringbuf),
				 "/proc/%s/task",
				 procdirent->d_name);
			if ((tasks = opendir(stringbuf)) != NULL) {
				while ((taskdirent = readdir(tasks))) {
					if (is_all_decimal(taskdirent->d_name)
					    && (sscanf(taskdirent->d_name,
						       "%u", &tid) == 1)) {
						memset(&currproc, 0,
						 sizeof(struct
						 foreign_thread_desc));
						currproc.pid = pid;
						currproc.tid = tid;
						snprintf(stringbuf,
						 sizeof(stringbuf),
						 "/proc/%s/task/%s/status",
							 procdirent->d_name,
							 taskdirent->d_name);
						sfile = fopen(stringbuf, "rt");
						if (sfile != NULL) {
							while (fgets(stringbuf,
							sizeof(stringbuf),
							     sfile) != NULL)
							update_proc_status(
								&currproc,
								stringbuf,
								pid, tid,
								cmd);
							fclose(sfile);
						}
						snprintf(stringbuf,
							sizeof(stringbuf),
							"/proc/%s/task/%s/stat",
							procdirent->d_name,
							taskdirent->d_name);
						sfile = fopen(stringbuf, "rt");
						if (sfile != NULL) {
							if (fgets(stringbuf,
							  sizeof(stringbuf),
							  sfile) != NULL)
							get_proc_stat(
								&currproc,
								stringbuf,
								pid, tid,
								cmd);
							fclose(sfile);
						}
						proctable_add_thread(&currproc);
#if DEBUG_ISOL_NAMES
						if (currproc.name != NULL)
							free(currproc.name);
#endif
					}
				}
				closedir(tasks);
			}
		}
	}
	closedir(processes);
	cleanup_threads();
	if (cmd & ISOL_PROC_LIST_CMD_PUSH_AWAY) {
		int i;
		pid_t my_pid;
		cpu_set_t overlap_cpuset, schedule_cpuset;

		my_pid = getpid();
		for (i = 0; i < proctable_size; i++) {
			/*
			  Don't modify parameters of isolated threads,
			  however do not exclude the main thread.
			*/
			if ((proctable[i].isolated_thread == NULL)
			    && ((proctable[i].pid != my_pid)
				|| (proctable[i].tid == my_pid))
			    &&
			    /*
			      Only change scheduling of processes and threads
			      that are not bound to a single CPU.
			    */
			    (CPU_COUNT(&proctable[i].cpus_allowed) > 1)) {
				CPU_AND(&overlap_cpuset,
					&proctable[i].cpus_allowed,
					&_global_isol_cpuset);
				if (CPU_COUNT(&overlap_cpuset) != 0) {
					/*
					  Some CPUs in this set overlap with
					  CPUS used for isolation.
					*/
					CPU_XOR(&schedule_cpuset,
						&proctable[i].cpus_allowed,
						&overlap_cpuset);
					/* Now they are all cleared */

					/*
					  It's possible that we have no CPUs
					  left, check for that
					*/
					if (CPU_COUNT(&schedule_cpuset) == 0) {
						/*
						  Allow all CPUs that are not
						  going to be used for
						  isolation.
						*/
						CPU_OR(&schedule_cpuset,
						       &schedule_cpuset,
						       &_global_nonisol_cpuset);
					}
					sched_setaffinity(proctable[i].tid,
							  sizeof(cpu_set_t),
							  &schedule_cpuset);
				}
			}
		}
	}
#if DEBUG_ISOL_VERBOSE
	if (cmd & ISOL_PROC_LIST_CMD_SHOW) {
		int i;
		for (i = 0; i < proctable_size; i++) {
			if (CPU_ISSET(proctable[i].cpu, cpuset))
				show_thread_desc(&proctable[i]);
		}
	}
#endif
	return 0;
}

/*
 * Thread states.
 */
enum memipc_thread_state {
	MEMIPC_STATE_OFF = 0,
	MEMIPC_STATE_STARTED,
	MEMIPC_STATE_READY,
	MEMIPC_STATE_LAUNCHING,
	MEMIPC_STATE_LAUNCHED,
	MEMIPC_STATE_RUNNING,
	MEMIPC_STATE_TMP_EXITING_ISOLATION,
	MEMIPC_STATE_EXITING_ISOLATION,
	MEMIPC_STATE_LOST_ISOLATION
};

#if DEBUG_ISOL_VERBOSE
static const char *memipc_thread_state_names[] = {
	"Off",
	"Started",
	"Ready",
	"Launching",
	"Launched",
	"Running",
	"Temporarily exiting isolation",
	"Exiting isolation",
	"Lost isolation"
};
#endif

/*
 *  Managed thread descriptor.
 */
struct memipc_thread_params {
	int index; /* Index in array, used for referencing */
	int cpu; /* CPU where thread can run isolated. This is set once
		    at the moment of system initialization */
	/* Thread IDs. Updated when threads start */
	pthread_t thread_id; /* Thread ID as seen by pthread */
	long pid; /* Process ID as seen by kernel */
	long tid; /* Thread ID as seen by kernel (this is not very
		     portable, and implementation is specific
		     to a particular implementation) */
	/* Atomically accessed counters and flags */
	int claim_counter; /* accessed atomically */
	char isolated; /* isolation state, accessed atomically, values:
			  0 - failure, 1 - not isolated, 2 - isolated or calling
			  isolation entry procedure.
			  Value does not reflect actual isolation while the
			  memipc_thread_state is
			  MEMIPC_STATE_TMP_EXITING_ISOLATION */

	/* Manager's state machine */
	enum memipc_thread_state state; /* accessed only by manager */
	char exit_request;              /* accessed only by manager */
	struct timespec isol_exit_time; /* accessed only by manager */

	/* Memory-mapped IPC */
	char *memipc_name;
	int memipc_fd;
	/* Master output, slave input, master view */
	struct memipc_area *m_memipc_mosi;
	/* Master input, slave output, master view */
	struct memipc_area *m_memipc_miso;
	/* Master output, slave input, slave view */
	struct memipc_area *s_memipc_mosi;
	/* Master input, slave output, slave view */
	struct memipc_area *s_memipc_miso;
	/* Pointer to local memipc_check_signal */
	volatile unsigned char *memipc_check_signal_ptr;
	volatile int *counter_ptr;

	/* Functions and data pointers for managed startup */
	void *(*init_routine) (void *); /* used only for managed startup */
	void *(*start_routine) (void *); /* used only for managed startup */
	void *userdata; /* used only for managed startup */

	/* Manager's references to processes and timers */
	struct foreign_thread_desc *foreign_desc; /* "Foreign thread"
						     descriptor,
						     if present */
	struct isol_linux_timer *timers; /* Timers list */
	int64_t lasttimer; /* Last timer expiration in nanoseconds,
			      or KTIME_MAX */
	int64_t updatetimer; /* Last time timers were updated, in nanoseconds,
				or KTIME_MAX */
};

static inline struct memipc_area *get_s_memipc_mosi(struct
						    memipc_thread_params
						    *arg)
{
	return arg->s_memipc_mosi;
}

/*
 *  Handler for requests in a slave/managed thread.
 */
static void memipc_slave_handle_request(int read_req_type,
					ssize_t read_req_size,
					unsigned char *memipc_read_buffer,
					struct memipc_thread_params *thread)
{
	char two = 2, zero = 0;
	switch (read_req_type) {
	case MEMIPC_REQ_NONE:
		/* Do nothing */
		break;
	case MEMIPC_REQ_INIT:
		/* Do nothing, we are the thread. */
		break;
	case MEMIPC_REQ_START_READY:
		/* Do nothing, we are the thread. */
		break;
	case MEMIPC_REQ_START_LAUNCH:
		/* Enter isolation. */
		memipc_thread_launch_confirmed = 0;
#ifdef DEBUG_LOG_ISOL_CHANGES
		write(1, "\nSTART_L, isolated = 2\n", 23);
#endif
		__atomic_store(&thread->isolated,
			       &two,
			       __ATOMIC_SEQ_CST);
		memipc_check_signal = 0;
		if (start_isolation(thread->cpu)) {
#ifdef DEBUG_LOG_ISOL_CHANGES
			write(1, "\nSTART_L, isolated = 0\n", 23);
#endif
			__atomic_store(&thread->isolated,
				       &zero,
				       __ATOMIC_SEQ_CST);
			while (memipc_add_req(thread->s_memipc_miso,
					      MEMIPC_REQ_START_LAUNCH_FAILURE,
					      0, NULL));
		} else {
			while (memipc_add_req(thread->s_memipc_miso,
					      MEMIPC_REQ_START_LAUNCH_DONE,
					      0, NULL));
		}
		break;
	case MEMIPC_REQ_START_LAUNCH_DONE:
		/* Do nothing, we are the thread. */
		break;
	case MEMIPC_REQ_START_LAUNCH_FAILURE:
		/* Do nothing, we are the thread. */
		break;
	case MEMIPC_REQ_START_CONFIRMED:
		/* Manager confirmed start, continue */
		memipc_thread_launch_confirmed = 1;
		break;
	case MEMIPC_REQ_TERMINATE:
		/* Should terminate. */
		memipc_thread_continue_flag = 0;
		break;
	case MEMIPC_REQ_EXIT_ISOLATION:
		/* Exit isolation. */
		exit_isolation();
		break;
	case MEMIPC_REQ_EXITING:
		/* Do nothing, we are the thread. */
		break;
	case MEMIPC_REQ_LEAVE_ISOLATION:
		/* Do nothing, we are the thread */
		break;
	case MEMIPC_REQ_OK_LEAVE_ISOLATION:
		/* Manager confirmed that this thread can now exit isolation */
		memipc_thread_ok_leave_flag = 1;
		break;
	case MEMIPC_REQ_PING:
		/* Should send MEMIPC_REQ_PONG back. */
		break;
	case MEMIPC_REQ_PONG:
		/* Do nothing, we are the thread. */
		break;
	case MEMIPC_REQ_CMD:
		/* Some command handling may be added here later. */
		break;
	case MEMIPC_REQ_PRINT:
		/* Do nothing, we are the thread. */
		break;
	default:
		/* Invalid request */
		break;
	}
}

/*
 *  Call this function in the main loop of the slave/managed thread.
 */
int memipc_thread_pass(struct memipc_thread_params *params)
{
	unsigned char memipc_read_buffer[AREA_SIZE];
	enum memipc_req_type read_req_type;
	ssize_t read_req_size;
#if ISOLATION_MONITOR_IN_SLAVE
	char isolated_state, one = 1;
	int rv, rcode_value = -1;
	struct tx_text tx;
	struct rx_buffer rx;

	rx.input_buffer = NULL;
	__atomic_load(&params->isolated,
		      &isolated_state,
		      __ATOMIC_SEQ_CST);
	if (isolated_state == 0 && memipc_thread_launch_confirmed != 0) {
		memipc_thread_launch_confirmed = 0;
#ifdef DEBUG_LOG_ISOL_CHANGES
		write(1, "\nPASS   , isolated = 1\n", 23);
#endif
		__atomic_store(&params->isolated,
			       &one,
			       __ATOMIC_SEQ_CST);
		if (memipc_thread_fd >= 0) {
			/* Exit from isolation, if still in isolation mode */
			prctl(PR_SET_TASK_ISOLATION, 0, 0, 0, 0);

			tx_init(&tx);
			rv = init_rx_buffer(&rx)
				|| tx_add_text(&tx, "taskisolfail\n")
				|| send_tx_fd_persist(memipc_thread_fd, &tx)
				|| ((rcode_value = read_rx_data(&rx,
							memipc_thread_fd,
								NULL)) != 220);
		} else
			rv = 1;
		if (rx.input_buffer != NULL) {
			free_rx_buffer(&rx);
			rx.input_buffer = NULL;
		}
		if (rv != 0)
			while (memipc_add_req(params->s_memipc_miso,
					      MEMIPC_REQ_START_LAUNCH_FAILURE,
					      0, NULL));
	}
#endif
	read_req_size = sizeof(memipc_read_buffer);
	read_req_type = MEMIPC_REQ_NONE;
	if (memipc_get_req(params->s_memipc_mosi,
			   &read_req_type,
			   &read_req_size,
			   memipc_read_buffer) == 0)
		memipc_slave_handle_request(read_req_type,
					    read_req_size,
					    memipc_read_buffer,
					    params);
	return memipc_thread_continue_flag;
}

/*
 *  Same as above, except for the current thread.
 */
int memipc_thread_pass_default(void)
{
	if (memipc_thread_self == NULL)
		return 0;
#if ISOLATION_MONITOR_IN_SLAVE
	char isolated_state;

	__atomic_load(&memipc_thread_self->isolated,
		      &isolated_state,
		      __ATOMIC_SEQ_CST);
	if ((isolated_state == 0) || memipc_check_newdata())
#else
		if (memipc_check_newdata())
#endif
			return memipc_thread_pass(memipc_thread_self);
		else
			return memipc_thread_continue_flag;
}

/*
 * Thread startup function.
 */
static void *memipc_thread_startup(void *arg_data)
{
	struct memipc_thread_params *params =
		(struct memipc_thread_params*)arg_data;
	void *retval = NULL;

	memipc_my_pid = params->thread_id;
	memipc_thread_self = params;
	params->s_memipc_mosi->reader = memipc_my_pid;
	params->s_memipc_miso->writer = memipc_my_pid;
	params->memipc_check_signal_ptr = &memipc_check_signal;
	params->counter_ptr = NULL;
	memipc_check_signal = 0;
	unsigned char message[]="Thread started\n";
	memipc_thread_launch_confirmed = 0;
	memipc_thread_continue_flag = 1;
#if ISOLATION_MONITOR_IN_SLAVE
	char one = 1;
#ifdef DEBUG_LOG_ISOL_CHANGES
	write(1, "\nTHR_STA, isolated = 1\n", 23);
#endif
	__atomic_store(&params->isolated,
		       &one,
		       __ATOMIC_SEQ_CST);
#endif
	/* Print startup message */
	while (memipc_add_req(params->s_memipc_miso, MEMIPC_REQ_PRINT,
			      strlen((char*)message), message));
	/* Call initialization, if any */
	if (params->init_routine)
		retval=params->init_routine(params->userdata);

	/* Ready for isolation */
	while (memipc_add_req(params->s_memipc_miso, MEMIPC_REQ_START_READY,
			      0, NULL));

	/* Talk to the manager until entered isolation */
	do {
		memipc_thread_pass(params);
	}
	while ((memipc_thread_launch_confirmed == 0)
	       && memipc_thread_continue_flag);

	if (memipc_thread_continue_flag) {
		/* At this point thread is in isolated state */

		/* Call the thread start routine, imitating pthread library */
		if (params->start_routine)
			retval=params->start_routine(params->userdata);
		else
			retval=NULL;
	}

	/* Exiting */
	prctl(PR_SET_TASK_ISOLATION, 0, 0, 0, 0);
#if ISOLATION_MONITOR_IN_SLAVE
	char zero = 0;
#ifdef DEBUG_LOG_ISOL_CHANGES
	write(1, "\nTHR_STA, isolated = 0\n", 23);
#endif
	__atomic_store(&params->isolated,
		       &zero,
		       __ATOMIC_SEQ_CST);
#endif
	while (memipc_add_req(params->s_memipc_miso, MEMIPC_REQ_EXITING,
			      0, NULL));
	if (memipc_thread_fd >= 0) {
		close(memipc_thread_fd);
		memipc_thread_fd = -1;
	}
	return retval;
}

/*
 * Request isolation exit and wait for acknowledgement.
 * This function must be called before leaving isolation
 * and possibly calling memipc_isolation_announce_exit()
 */
void memipc_isolation_request_leave_isolation(void)
{
	int counter = 0;

	if (memipc_thread_self == NULL)
		return;

	memipc_thread_ok_leave_flag = 0;
	while (memipc_add_req(memipc_thread_self->s_memipc_miso,
			      MEMIPC_REQ_LEAVE_ISOLATION,
			      0, NULL));
    do {
	    memipc_thread_pass(memipc_thread_self);
	    if (++counter > 1000000) {
		    counter = 0;
		    memipc_add_req(memipc_thread_self->s_memipc_miso,
				   MEMIPC_REQ_LEAVE_ISOLATION,
				   0, NULL);
	    }
    }
    while ((memipc_thread_ok_leave_flag == 0)
	   && memipc_thread_continue_flag);
}

/*
 * Notify the manager about thread exit. This function must be called before
 * thread exit.
 */
void memipc_isolation_announce_exit(void)
{
	if (memipc_thread_self == NULL)
		return;

	while (memipc_add_req(memipc_thread_self->s_memipc_miso,
			      MEMIPC_REQ_EXITING,
			      0, NULL));
}

/*
 * printf() replacement for isolated mode. Will return a negative number if
 * there is not enough space in buffer, retry if necessary.
 */
int memipc_isolation_printf(const char *fmt, ...)
{
	unsigned char buffer[2048];
	va_list va;
	int l;

	va_start(va, fmt);
	l = vsnprintf((char *)buffer, sizeof(buffer), fmt, va);
	va_end(va);

	if (memipc_thread_self == NULL)
		return write(1, buffer, l);
	else
		if (memipc_add_req(memipc_thread_self->s_memipc_miso,
				   MEMIPC_REQ_PRINT,
				   l, buffer))
			return -EAGAIN;
	return l;
}

/*
  Various globals.
*/
static struct memipc_thread_params *_global_isolated_threads = NULL;
static int _global_isolated_thread_count = 0;
static int _global_isolated_threads_timeout_started = 0;
static time_t _global_isolated_threads_start_time = 0;
static time_t _global_isolated_threads_start_timeout = 20;
static time_t _global_isolated_threads_restart_delay = 3;

/*
 * Find descriptor of a managed thread with a given ID.
 */
struct memipc_thread_params *isolation_find_thread(pthread_t thread_id)
{
	int i;
	struct memipc_thread_params *threads;
	int threads_count;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;

	if (thread_id == 0)
		return NULL;

	for (i = 0; i < threads_count; i++) {
		if (threads[i].thread_id == thread_id)
			return &threads[i];
	}
	return NULL;
}

/*
 * Add a timer to the managed thread descriptor.
 */
static int memipc_add_timer_to_desc(int cpu,
#if DEBUG_ISOL_NAMES
				    const char *addr,
				    const char *handler,
#endif
				    enum isol_timer_type timer_type,
				    int64_t last_updated,
				    int64_t expires)
{
	int i, l
#if DEBUG_ISOL_NAMES
		, laddr
#endif
		;
	struct memipc_thread_params *threads;
	int threads_count;
	struct isol_linux_timer *t;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;

	for (i = 0; i < threads_count; i++) {
		if (threads[i].cpu == cpu) {
#if DEBUG_ISOL_NAMES
			laddr = strlen(addr);
#endif
			l = sizeof(struct isol_linux_timer)
#if DEBUG_ISOL_NAMES
				+ laddr + strlen(handler)
				+ 2
#endif
				;
			t = (struct isol_linux_timer *)malloc(l);
			if (t == NULL)
				return -1;
#if DEBUG_ISOL_NAMES
			t->addr = ((char*)t)
				+ sizeof(struct isol_linux_timer);
			t->handler = ((char*)t)
				+ sizeof(struct isol_linux_timer)
				+ laddr + 1;
			strcpy(t->addr, addr);
			strcpy(t->handler, handler);
#endif
			t->timer_type = timer_type;
			t->last_updated = last_updated;
			t->expires = expires;
			t->next = threads[i].timers;
			threads[i].timers = t;
			return 0;
		}
	}
	return -1;
}

#if 0
/*
 * Remove all timers from a managed thread descriptor.
 */
static void memipc_remove_timers_from_desc(int cpu)
{
	int i;
	struct memipc_thread_params *threads;
	int threads_count;
	struct isol_linux_timer *t, *tnext;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;

	for (i = 0; i < threads_count; i++) {
		if (threads[i].cpu == cpu) {
			t = threads[i].timers;
			threads[i].timers = NULL;
			while (t != NULL) {
				tnext = t->next;
				free(t);
				t = tnext;
			}
			return;
		}
	}
}
#endif

/*
 * Remove all timers from all managed thread descriptors.
 */
static void memipc_remove_timers_from_all_desc(void)
{
	int i;
	struct memipc_thread_params *threads;
	int threads_count;
	struct isol_linux_timer *t, *tnext;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;

	for (i = 0; i < threads_count; i++) {
		t = threads[i].timers;
		threads[i].timers = NULL;
		while (t != NULL) {
			tnext = t->next;
			free(t);
			t = tnext;
		}
	}
}

#if DEBUG_ISOL_VERBOSE
/*
 * Show all timers on all managed thread descriptors.
 */
static void show_all_timers_by_desc(FILE *f, cpu_set_t *cpuset)
{
	int i;
	struct memipc_thread_params *threads;
	int threads_count;
	struct isol_linux_timer *t;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;

	for (i = 0; i < threads_count; i++) {
		t = threads[i].timers;
		if ((t != NULL)
		    && ((cpuset == NULL)
			|| CPU_ISSET(threads[i].cpu, cpuset))) {
			fprintf(f, "Timers for CPU %d:\n", threads[i].cpu);
			while (t != NULL) {
#if DEBUG_ISOL_NAMES
				fprintf(f,
					" Timer %s, at %s, handler %s, "
					"expires at %lu\n",
					isol_timer_type_name[t->timer_type],
					t->addr,
					t->handler,
					(uint64_t) t->expires);
#else
				fprintf(f,
					" Timer %s, "
					"expires at %lu\n",
					isol_timer_type_name[t->timer_type],
					(uint64_t) t->expires);
#endif
				t = t->next;
			}
		}
	}
}
#endif

/*
 * Find managed thread by "foreign" descriptor and establish references
 * between it and managed thread descriptor.
 */
static int memipc_attach_thread_to_desc(struct foreign_thread_desc *desc)
{
	int i;
	struct memipc_thread_params *threads;
	int threads_count;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;

	if ((desc->pid <= 0) || (desc->tid <= 0))
		return -1;

	for (i = 0; i < threads_count; i++) {
		if ((threads[i].pid == desc->pid)
		    && (threads[i].tid == desc->tid)) {
			desc->isolated_thread = &threads[i];
			threads[i].foreign_desc = desc;
			return 0;
		}
	}
    return 1;
}

/*
 * Detach "foreign" and managed thread descriptors.
 */
static void memipc_detach_thread_from_desc(struct foreign_thread_desc *desc)
{
	if (desc->isolated_thread != NULL) {
		desc->isolated_thread->foreign_desc = NULL;
		desc->isolated_thread = NULL;
	}
}

/*
 * Update managed thread descriptor reference to corresponding
 * "foreign" thread.
 *
 * Managed thread descriptors are allocated once, "foreign" thread
 * descriptors move when processes and threads are re-scanned.
 */
static void memipc_update_foreign_thread(struct foreign_thread_desc *desc)
{
	if (desc->isolated_thread != NULL)
		desc->isolated_thread->foreign_desc = desc;
}

#if DEBUG_ISOL_VERBOSE
/*
 * Return a name of a thread state.
 */
static const char *memipc_thread_state_name(struct memipc_thread_params *s)
{
	static const char *invalid = "Invalid";
	if (s
	    && (s->state >= 0)
	    && (s->state < (sizeof(memipc_thread_state_names)
			    / sizeof(memipc_thread_state_names[0]))))
		return memipc_thread_state_names[s->state];
	else
		return invalid;
}
#endif

/*
 * The following is a "claim CPU" mechanism used to connect the
 * threads to both CPUs and memipc areas. Only CPUs that can be
 * isolated, can be claimed within this mechanism. Nothing actually
 * happens to the process or thread calling this, the mechanism only
 * reserves CPUs and descriptors.
 *
 * This is necessary for kinds of initialization that require a CPU to
 * be chosen for some initialization that happens before entering
 * isolated mode.
 */

/*
 * Claim given CPU, or any CPU.
 *
 * Negative argument means, the first available CPU.
 * Returns a thread descriptor or NULL if not available.
 */
struct memipc_thread_params *isolation_claim_cpu(int cpu)
{
	int i, one = 1, orig_claim_counter;
	struct memipc_thread_params *threads;
	int threads_count;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;

	for (i = 0; i < threads_count; i++) {
		if ((cpu < 0) || (threads[i].cpu == cpu)) {
			orig_claim_counter =
				__atomic_fetch_add(&threads[i].claim_counter,
						   one, __ATOMIC_SEQ_CST);
			if (orig_claim_counter == 0)
				return &threads[i];
			else
				__atomic_fetch_sub(&threads[i].claim_counter,
						   one, __ATOMIC_SEQ_CST);
		}
	}
	return NULL;
}

/*
 * Release CPU of a given thread.
 *
 * This function should be only called once on a thread with claimed
 * CPU.
 */
void isolation_release_cpu(struct memipc_thread_params *thread)
{
	int one = 1;
	if (thread == NULL)
		return;
	__atomic_fetch_sub(&thread->claim_counter,
			   one, __ATOMIC_SEQ_CST);
}

static void memipc_isolation_process_ready_launch(cpu_set_t *timers_cpuset,
						  int64_t now);

/*
 * Handler for requests in a master/manager thread.
 */
void memipc_master_handle_request(int read_req_type,
				  ssize_t read_req_size,
				  unsigned char *memipc_read_buffer,
				  struct memipc_thread_params *thread)
{
	char buffer[19];
	char zero = 0;
	static int thread_last_cpu = -1, last_newline = 1;
	int client_index;

	(void)zero;

	switch (read_req_type) {
	case MEMIPC_REQ_NONE:
		fprintf(stderr,
		"Manager received MEMIPC_REQ_NONE from thread on CPU %d\n",
			thread->cpu);
		break;
	case MEMIPC_REQ_INIT:
		/* Start monitoring */
		CPU_SET(thread->cpu, &_global_running_cpuset);
		if (thread->state == MEMIPC_STATE_OFF)
			thread->state = MEMIPC_STATE_STARTED;
		break;
	case MEMIPC_REQ_START_READY:
		if ((size_t)read_req_size >= sizeof(thread->counter_ptr))
			memcpy(&thread->counter_ptr, memipc_read_buffer,
			       sizeof(thread->counter_ptr));
		thread->state = MEMIPC_STATE_READY;
#if DEBUG_ISOL_VERBOSE
		fprintf(stderr,
			"Thread on CPU %d ready\n", thread->cpu);
#endif
		{
			cpu_set_t timers_cpuset;
			int64_t now;
			/*
			  Get all CPUs with isolation and timers running
			  on them.
			*/
			process_all_timers(&timers_cpuset, &now);
			memipc_isolation_process_ready_launch(&timers_cpuset,
							      now);
		}
	    break;
	case MEMIPC_REQ_START_LAUNCH:
		/* Do nothing, we are the manager */
		break;
	case MEMIPC_REQ_START_LAUNCH_DONE:
		/* Should react to thread now running */
		if ((thread->state == MEMIPC_STATE_TMP_EXITING_ISOLATION)
		    || (thread->state == MEMIPC_STATE_EXITING_ISOLATION)) {
#if DEBUG_ISOL_VERBOSE
			fprintf(stderr,
			"Thread launch message arrived too late, CPU %d\n",
				thread->cpu);
#endif
		} else {
			thread->state = MEMIPC_STATE_LAUNCHED;
#if DEBUG_ISOL_VERBOSE
			fprintf(stderr,
				"Thread launch OK, CPU %d\n", thread->cpu);
#endif
		}
	    break;
	case MEMIPC_REQ_START_LAUNCH_FAILURE:
		/* Should react to the launch failure */
#if DEBUG_ISOL_VERBOSE
		fprintf(stderr,
			"Message: Isolation failure on CPU %d\n",
			thread->cpu);
#endif
	    /* Re-launch */
#if ISOLATION_MONITOR_IN_MASTER
		{
		char isolated_state;
		__atomic_load(&thread->isolated,
			      &isolated_state,
			      __ATOMIC_SEQ_CST);
		if (isolated_state == 0) {
			if ((thread->state
			     != MEMIPC_STATE_TMP_EXITING_ISOLATION)
			 && (thread->state
			     != MEMIPC_STATE_EXITING_ISOLATION)) {
				thread->state = MEMIPC_STATE_LOST_ISOLATION;
				clock_gettime(CLOCK_MONOTONIC,
					      &thread->isol_exit_time);
				if (memipc_add_req(thread->m_memipc_mosi,
						   MEMIPC_REQ_START_LAUNCH,
						   0, NULL) == 0) {
				        if (thread->counter_ptr != NULL)
						(*(thread->counter_ptr))++;
					thread->state = MEMIPC_STATE_LAUNCHING;
#if DEBUG_ISOL_VERBOSE
					fprintf(stderr,
					"Re-launching thread on CPU %d\n",
						thread->cpu);
#endif
				}
			}
		}
		}
#else
		if ((thread->state != MEMIPC_STATE_TMP_EXITING_ISOLATION)
		    && (thread->state != MEMIPC_STATE_EXITING_ISOLATION)) {
			thread->state = MEMIPC_STATE_LOST_ISOLATION;
			clock_gettime(CLOCK_MONOTONIC, &thread->isol_exit_time);
			if (memipc_add_req(thread->m_memipc_mosi,
					   MEMIPC_REQ_START_LAUNCH,
					   0, NULL) == 0) {
				if (thread->counter_ptr != NULL)
					(*(thread->counter_ptr))++;
				thread->state = MEMIPC_STATE_LAUNCHING;
#if DEBUG_ISOL_VERBOSE
				fprintf(stderr,
					"Re-launching thread on CPU %d\n",
					thread->cpu);
#endif
			}
		}
#endif
		break;
	case MEMIPC_REQ_START_CONFIRMED:
		/* Do nothing, we are the manager. */
		break;
	case MEMIPC_REQ_TERMINATE:
		/* Do nothing, we are the manager. */
		break;
	case MEMIPC_REQ_EXIT_ISOLATION:
		/* Do nothing, we are the manager. */
		break;
	case MEMIPC_REQ_EXITING:
		/* This is an equivalent of ISOL_SRV_CMD_TASKISOLFINISH
		   command */
		/* Finish, optionally join exiting thread */
		thread->state = MEMIPC_STATE_OFF;
		thread->counter_ptr = NULL;
		thread->exit_request = 0;
		CPU_CLR(thread->cpu, &_global_running_cpuset);
		if (thread->foreign_desc != NULL)
			memipc_detach_thread_from_desc(thread->foreign_desc);
		if (thread->pid == getpid()) {
			/* Thread from the same process, join it. */
			pthread_join(thread->thread_id, NULL);
		}
#if ISOLATION_MONITOR_IN_MASTER
#ifdef DEBUG_LOG_ISOL_CHANGES
		write(1, "\nREQ_EXI, isolated = 0\n", 23);
#endif
		__atomic_store(&thread->isolated,
			       &zero,
			       __ATOMIC_SEQ_CST);
#endif
		thread->start_routine = NULL;
		thread->userdata = NULL;
		thread->lasttimer = KTIME_MAX;
		thread->updatetimer = KTIME_MAX;
		isolation_release_cpu(thread);
		client_index = get_client_index((void*)thread);
		if (client_index >= 0) {
			/* Disconnect a task from a client */
			set_client_task(client_index, NULL);
			/* End client session immediately. */
			close_client_connection(client_index);
		}
#if DEBUG_ISOL_VERBOSE
		fprintf(stderr, "Thread on CPU %d exited\n", thread->cpu);
#endif
		break;
	case MEMIPC_REQ_LEAVE_ISOLATION:
	    /* Acknowledge request to leave isolation, stop watching thread */
		if (memipc_add_req(thread->m_memipc_mosi,
				   MEMIPC_REQ_OK_LEAVE_ISOLATION,
				   0, NULL) == 0) {
			thread->counter_ptr = NULL;
			thread->state = MEMIPC_STATE_EXITING_ISOLATION;
#if DEBUG_ISOL_VERBOSE
			fprintf(stderr,
				"Thread on CPU %d leaving isolation\n",
				thread->cpu);
#endif
		}
		break;
	case MEMIPC_REQ_OK_LEAVE_ISOLATION:
		/* Do nothing, we are the manager */
		break;
	case MEMIPC_REQ_PING:
		/* Should send MEMIPC_REQ_PONG back. */
		break;
	case MEMIPC_REQ_PONG:
		/* Nothing for now, start next watchdog cycle, if supported. */
		break;
	case MEMIPC_REQ_CMD:
		/* Some command handling may be added here later. */
		break;
	case MEMIPC_REQ_PRINT:
		/* Print the message on standard output */
		if (thread_last_cpu != thread->cpu) {
			thread_last_cpu = thread->cpu;
			snprintf(buffer, sizeof(buffer), "\r\nCPU %2d: ",
				 thread->cpu);
			write(1, buffer + last_newline * 2,
			      strlen(buffer) - last_newline * 2);
		}
		write(1, memipc_read_buffer, read_req_size);
		if (read_req_size > 0)
			last_newline = memipc_read_buffer[read_req_size - 1]
				== '\n';
		break;
	default:
		/* Invalid request */
		fprintf(stderr,
			"Manager received invalid request type %d, size %lu "
			"from thread on CPU %d\n",
			read_req_type, read_req_size, thread->cpu);
		break;
	}
}

/*
 * Determine if any I/O is expected for the purpose of assisted
 * entering or exiting isolation.
 */
static int memipc_isolation_io_expected(void)
{
	int i, io_expected_count;
	struct memipc_thread_params *threads;
	int threads_count;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;

	for (i = 0, io_expected_count = 0; i < threads_count; i++) {
		if ((threads[i].state == MEMIPC_STATE_STARTED)
		    || (threads[i].state == MEMIPC_STATE_READY)
		    || (threads[i].state == MEMIPC_STATE_LAUNCHING)
		    || (threads[i].state == MEMIPC_STATE_LAUNCHED)
		    || (threads[i].state == MEMIPC_STATE_TMP_EXITING_ISOLATION)
		    || (threads[i].state == MEMIPC_STATE_EXITING_ISOLATION)
		    || (threads[i].state == MEMIPC_STATE_LOST_ISOLATION)
		    || (threads[i].exit_request != 0))
			io_expected_count++;
	}
	return io_expected_count;
}

/*
 * Launch isolation in threads if and when they should be isolated,
 * confirm entering isolation if there are no timers left.
 */
static void memipc_isolation_process_ready_launch(cpu_set_t *timers_cpuset,
						  int64_t now)
{
	int i, ready_count, needs_start_count,
		show_running_threads_flag = 0
#if DEBUG_ISOL_VERBOSE
		, show_running_timers_flag = 0
#endif
		;
#if 0
	, errflag = 0;
#endif
	;
	cpu_set_t overlap_cpuset;
	struct memipc_thread_params *threads;
	int threads_count;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;

	for (i = 0, ready_count = 0, needs_start_count = 0;
	     i < threads_count; i++) {
		if ((threads[i].state == MEMIPC_STATE_READY)
		    || (threads[i].state == MEMIPC_STATE_LAUNCHED)
		    || (threads[i].state == MEMIPC_STATE_TMP_EXITING_ISOLATION)
		    || (threads[i].state == MEMIPC_STATE_LOST_ISOLATION)
		    || (threads[i].exit_request != 0))
			needs_start_count++;

		if ((threads[i].state == MEMIPC_STATE_READY)
		    || (threads[i].state == MEMIPC_STATE_TMP_EXITING_ISOLATION)
		    || (threads[i].state == MEMIPC_STATE_EXITING_ISOLATION)
		    || (threads[i].state == MEMIPC_STATE_LOST_ISOLATION)
		    || (threads[i].state == MEMIPC_STATE_LAUNCHING)
		    || (threads[i].state == MEMIPC_STATE_LAUNCHED)
		    || (threads[i].state == MEMIPC_STATE_RUNNING))
			ready_count++;
	}

	if (needs_start_count == 0)
		goto last_actions;

	/*
	  If not all threads are ready, check timeout.
	  If it expired, proceed anyway
	*/
	if (ready_count < threads_count) {
		if (_global_isolated_threads_timeout_started == 0)
			goto last_actions;

		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		if ((ts.tv_sec - _global_isolated_threads_start_time)
		    < _global_isolated_threads_start_timeout)
			goto last_actions;
	}

	/* Should issue the request to launch */
	for (i = 0, show_running_threads_flag = 0
#if DEBUG_ISOL_VERBOSE
		    , show_running_timers_flag = 0
#endif
		    ;
	    i < threads_count; i++) {
		struct timespec ts;

		/*
		  Send exit requests if there are any pending.
		  This happens regardless of state.
		*/
		if (threads[i].exit_request != 0) {
			if (memipc_add_req(threads[i].m_memipc_mosi,
					   MEMIPC_REQ_TERMINATE,
					   0, NULL) == 0) {
				if (threads[i].counter_ptr != NULL)
					(*(threads[i].counter_ptr))++;
				threads[i].exit_request = 0;
			}
		}

		/* Process thread state. */
		switch (threads[i].state) {
		case MEMIPC_STATE_TMP_EXITING_ISOLATION:
			clock_gettime(CLOCK_MONOTONIC, &ts);
			if ((ts.tv_sec - threads[i].isol_exit_time.tv_sec)
			    > _global_isolated_threads_restart_delay) {
				if (memipc_add_req(threads[i].m_memipc_mosi,
						   MEMIPC_REQ_START_LAUNCH,
						   0, NULL) == 0) {
					if (threads[i].counter_ptr != NULL)
						(*(threads[i].counter_ptr))++;
					threads[i].state =
						MEMIPC_STATE_LAUNCHING;
#if DEBUG_ISOL_VERBOSE
					fprintf(stderr,
					"Re-launching thread after leaving"
						" and waiting, CPU %d\n",
						threads[i].cpu);
#endif
				}
#if 0
				else
					errflag = 1;
#endif
			}
			break;
		case MEMIPC_STATE_READY:
		case MEMIPC_STATE_LOST_ISOLATION:
			if (memipc_add_req(threads[i].m_memipc_mosi,
					   MEMIPC_REQ_START_LAUNCH,
					   0, NULL) == 0) {
				if (threads[i].counter_ptr != NULL)
					(*(threads[i].counter_ptr))++;
				threads[i].state = MEMIPC_STATE_LAUNCHING;
			}
#if 0
			else
				errflag = 1;
#endif
			break;
		case MEMIPC_STATE_LAUNCHED:
			CPU_AND(&overlap_cpuset,
				timers_cpuset,
				&_global_running_cpuset);
			if (CPU_COUNT(&overlap_cpuset) == 0) {
				/*
				  There are no timers left on all CPUs
				  that run isolated threads, so it's safe
				  to continue.
				*/
#if DEBUG_ISOL_STARTUP_MESSAGE
				fprintf(stderr,
					"Thread on CPU %d is running "
					"in isolated mode\n",
					threads[i].cpu);
#endif
				if (memipc_add_req(threads[i].m_memipc_mosi,
						   MEMIPC_REQ_START_CONFIRMED,
						   0, NULL) == 0) {
					if (threads[i].counter_ptr != NULL)
						(*(threads[i].counter_ptr))++;
					threads[i].state = MEMIPC_STATE_RUNNING;
				}
#if 0
				else
					errflag = 1;
#endif
			} else {
				if (CPU_ISSET(threads[i].cpu, timers_cpuset)) {
					/*
					  Timers are running on isolated thread
					*/
#if DEBUG_ISOL_VERBOSE
					fprintf(stderr,
					"Timers are present on CPU %d, "
					"requesting exit from isolation\n",
					threads[i].cpu);
#endif
					if (memipc_add_req(
						threads[i].m_memipc_mosi,
						MEMIPC_REQ_EXIT_ISOLATION,
						0, NULL) == 0) {
						if (threads[i].counter_ptr
						    != NULL)
						(*(threads[i].counter_ptr))++;
						threads[i].state
					= MEMIPC_STATE_TMP_EXITING_ISOLATION;
						clock_gettime(CLOCK_MONOTONIC,
						&threads[i].isol_exit_time);
					}
#if DEBUG_ISOL_VERBOSE
					show_running_timers_flag = 1;
#endif
				} else {
					/*
					  Some timers are still running, check
					  if they are relevant for threads that
					  are running right now.
					*/
					int64_t remaining;
					remaining =
					remaining_nsec_before_expiration(now);
#if DEBUG_ISOL_VERBOSE
					fprintf(stderr, "CPUs with timers: ");
					print_cpuset(stderr, timers_cpuset);
#endif
					if (remaining != KTIME_MAX) {
						/*
						  Some are still in progress,
						  keep waiting.
						*/
#if DEBUG_ISOL_VERBOSE
						fprintf(stderr,
						", %ld ns left, thread on "
							"CPU %d should wait\n",
							remaining,
							threads[i].cpu);
						show_running_timers_flag = 1;
#endif
					} else {
						/*
						  Everything relevant seemingly
						  expired, however timers are
						  still present in the list, so
						  keep waiting.
						*/
#if DEBUG_ISOL_VERBOSE
						fprintf(stderr,
					", thread on CPU %d should wait\n",
							threads[i].cpu);
						show_running_timers_flag = 1;
#endif
					}
				}
				/*
				  Show currently running threads
				  once all our threads are processed.
				*/
				show_running_threads_flag = 1;
			}
			break;
		default:
			break;
		}
	}
    /* FIXME: we don't have usable criteria for stopping this yet */
#if 0
	if (errflag == 0)
		_global_isolated_threads_timeout_started = 0;
#endif

 last_actions:
#if DEBUG_ISOL_VERBOSE
	if (show_running_timers_flag)
		show_all_timers_by_desc(stderr, timers_cpuset);
#endif
	if (show_running_threads_flag) {
		/*
		  Show currently running threads, push threads away from
		  CPUs intended for isolation.
		*/
		process_all_threads(timers_cpuset,
#if DEBUG_ISOL_VERBOSE
				    ISOL_PROC_LIST_CMD_SHOW |
#endif
				    ISOL_PROC_LIST_CMD_PUSH_AWAY);
	} else {
		struct timespec ts;
		static time_t last_thread_scan = 0;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		if (((last_thread_scan == 0)
		     || (ts.tv_sec - last_thread_scan) > 3)) {
			/* Push threads away from CPUs intended
			   for isolation. */
			process_all_threads(timers_cpuset,
					    ISOL_PROC_LIST_CMD_PUSH_AWAY);
			clock_gettime(CLOCK_MONOTONIC, &ts);
			last_thread_scan = ts.tv_sec;
			if (last_thread_scan == 0)
				last_thread_scan = 1;
		}
	}
}

/*
 * Get the number of threads that can run isolated.
 */
int memipc_isolation_get_max_isolated_threads_count(void)
{
	return _global_isolated_thread_count;
}

/*
 * Terminate thread.
 */
void memipc_isolation_terminate_thread(struct memipc_thread_params *thread)
{
	thread->exit_request = 1;
}

/*
 * Terminate all threads.
 */
void memipc_isolation_terminate_all_threads(void)
{
	struct memipc_thread_params *threads;
	int threads_count;
	int i;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;
	for (i = 0; i < threads_count; i++)
		threads[i].exit_request = 1;
}

/*
 * Manager loop.
 */
int memipc_isolation_run_threads(void)
{
	struct memipc_thread_params *threads;
	int threads_count;
	int i, counter_threads_not_running, threads_were_running;
	int poll_timeout = 0;

	unsigned char memipc_read_buffer[AREA_SIZE];
	enum memipc_req_type read_req_type;
	ssize_t read_req_size;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;

	threads_were_running = 0;
	counter_threads_not_running = 0;
	while ((counter_threads_not_running != threads_count)
	       || (threads_were_running == 0)
	       || is_pending_data_present()) {
		isol_server_poll_pass(poll_timeout);
		counter_threads_not_running = 0;
		for (i = 0; i < threads_count; i++) {
			int claim_counter;
			__atomic_load(&threads[i].claim_counter,
				      &claim_counter,
				      __ATOMIC_SEQ_CST);
			if (claim_counter) {
#if ISOLATION_MONITOR_IN_MASTER
				if (threads[i].state != MEMIPC_STATE_OFF) {
					char isolated_state, zero = 0, one = 1;
					__atomic_load(&threads[i].isolated,
						      &isolated_state,
						      __ATOMIC_SEQ_CST);
					if (isolated_state == 0) {
#ifdef DEBUG_LOG_ISOL_CHANGES
						write(1,
						"\nMONITOR, isolated = 1\n",
						      23);
#endif
						__atomic_store(
						       &threads[i].isolated,
						       &one,
						       __ATOMIC_SEQ_CST);
						/* Should react to the launch
						   failure */
#if DEBUG_ISOL_VERBOSE
						fprintf(stderr,
				"Manager: Isolation failure on CPU %d\n",
							threads[i].cpu);
#endif
						/* Re-launch */
						if (memipc_add_req(
						threads[i].m_memipc_mosi,
						MEMIPC_REQ_START_LAUNCH,
						0, NULL) != 0) {
#ifdef DEBUG_LOG_ISOL_CHANGES
							write(1,
						"\nMONITOR, isolated = 0\n",
							      23);
#endif
							__atomic_store(
							&threads[i].isolated,
							&zero,
							__ATOMIC_SEQ_CST);
						} else {
							if (threads[i].
							    counter_ptr != NULL)
							(*(threads[i].
							   counter_ptr))++;
							threads[i].state
						= MEMIPC_STATE_LAUNCHING;
#if DEBUG_ISOL_VERBOSE
							fprintf(stderr,
					    "Re-launching thread on CPU %d\n",
								threads[i].cpu);
#endif
						}
					}
				}
#endif
				read_req_size = sizeof(memipc_read_buffer);
				read_req_type = MEMIPC_REQ_NONE;
				if (memipc_get_req(threads[i].m_memipc_miso,
						   &read_req_type,
						   &read_req_size,
						   memipc_read_buffer) == 0) {
				memipc_master_handle_request(read_req_type,
							     read_req_size,
							     memipc_read_buffer,
							     &threads[i]);
				}
			}
			/*
			  Check if the current thread is running, this will be
			  used it to determine if this loop should finish.
			*/
			if (threads[i].state == MEMIPC_STATE_OFF)
				counter_threads_not_running++;
			else
				threads_were_running = 1;
		}
		if (_global_isolated_threads_timeout_started) {
			cpu_set_t timers_cpuset;
			int64_t now;
			/*
			  Get all CPUs with isolation and timers running
			  on them.
			*/
			process_all_timers(&timers_cpuset, &now);
			memipc_isolation_process_ready_launch(&timers_cpuset,
							      now);
			if (memipc_isolation_io_expected() == 0)
				poll_timeout = ISOL_SERVER_IDLE_POLL_TIMEOUT;
			else
				poll_timeout = 0;
		}
	}
	return 0;
}

/*
 * Linux-specific thread ID conversion.
 *
 * This reproduces offsets in thread descriptors, and is very much
 * dependent on the internals of pthread implementation. It would be a
 * very bad idea to use, except that better alterhatives would have low
 * performance.
 *
 * Nevertheless, FIXME: we might benefit from using a more portable
 * alternative (at least it will get rid of offsets) that would rely on
 * name setting and lookup in /proc.
 */
static unsigned int isolation_get_tid(pthread_t thread)
{
	/* Mockup of a double-linked list used by pthread */
	struct mockup_list_head	{
		struct mockup_list_head *next;
		struct mockup_list_head *prev;
	};

	/* Mockup of a thread descriptor data structure. */
	struct mockup_pthread {
		void *__padding[24];
		struct mockup_list_head list;
		pid_t tid;
		pid_t pid;
	};

	const struct mockup_pthread *p;
	p = (const struct mockup_pthread *) thread;
	return (unsigned long) p->tid;
}

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
			    void *(*start_routine)(void*), void *arg)
{
	struct memipc_thread_params *thread;
	int retval;
#if ISOLATION_MONITOR_IN_MASTER
	char zero = 0, one = 1;
#endif

	thread = isolation_claim_cpu(cpu);
	if (thread == NULL)
		return -EINVAL;
	thread->init_routine = init_routine;
	thread->start_routine = start_routine;
	thread->userdata = arg;
#if ISOLATION_MONITOR_IN_MASTER
	/*
	  thread->isolated value 1 means that initialization is in progress,
	  and thread may be not in isolated state.
	*/
#ifdef DEBUG_LOG_ISOL_CHANGES
	write(1, "\nTHR_CRE, isolated = 1\n", 23);
#endif
	__atomic_store(&thread->isolated,
		       &one,
		       __ATOMIC_SEQ_CST);
#endif
	retval = pthread_create(&thread->thread_id, attr, memipc_thread_startup,
				thread);
	if (retval < 0) {
		thread->thread_id = 0;
		thread->pid = 0;
		thread->tid = 0;
		thread->lasttimer = KTIME_MAX;
		thread->updatetimer = KTIME_MAX;
#if ISOLATION_MONITOR_IN_MASTER
		/*
		  thread->isolated value 0 means that thread does not exist or
		  lost its isolated state.
		*/
#ifdef DEBUG_LOG_ISOL_CHANGES
		write(1, "\nTHR_CRE, isolated = 0\n", 23);
#endif
		__atomic_store(&thread->isolated,
			       &zero,
			       __ATOMIC_SEQ_CST);
#endif
		isolation_release_cpu(thread);
	} else {
		/*
		  The following two fields are filled _after_ the
		  start of the thread, don't rely on them being filled
		  from within the thread.
		*/
		thread->pid = (unsigned long) getpid();
		thread->tid = isolation_get_tid(thread->thread_id);

		thread->lasttimer = KTIME_MAX;
		thread->updatetimer = KTIME_MAX;
		CPU_SET(thread->cpu, &_global_running_cpuset);
		thread->state = MEMIPC_STATE_STARTED;
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		_global_isolated_threads_start_time = ts.tv_sec;
		_global_isolated_threads_timeout_started = 1;
	}
	return retval;
}

/*
 * Claim a CPU from a started thread.
 *
 * Thread may call this function.
 *
 * This is another way of starting a managed thread -- already existing thread
 * is attached to the managed environment. This can not be done in a thread
 * that is already managed.
 */
int isolation_connect_this_thread(int cpu)
{
	struct memipc_thread_params *thread;
	pthread_t thread_id;
	char one = 1;

	thread_id = pthread_self();
	if (memipc_thread_self != NULL)
		return -EEXIST;

	thread = isolation_claim_cpu(cpu);
	if (thread == NULL)
		return -EINVAL;

	/* Set per-thread environment and thread descriptor */
	memipc_my_pid = thread_id;
	memipc_thread_self = thread;
	thread->thread_id = thread_id;

	/* Those values are filled by the thread itself before sending
	   requests */
	thread->pid = (unsigned long) getpid();
	thread->tid = isolation_get_tid(thread_id);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	thread->s_memipc_mosi->reader = memipc_my_pid;
	thread->s_memipc_miso->writer = memipc_my_pid;
	thread->memipc_check_signal_ptr = &memipc_check_signal;
	thread->counter_ptr = NULL;
	memipc_check_signal = 0;
	/*
	  thread->isolated value 1 means that initialization is in progress,
	  and thread may be not in isolated state.
	*/
#ifdef DEBUG_LOG_ISOL_CHANGES
	write(1, "\nTHR_CON, isolated = 1\n", 23);
#endif
	__atomic_store(&thread->isolated,
		       &one,
		       __ATOMIC_SEQ_CST);

	/* Notify the manager */
	while (memipc_add_req(thread->s_memipc_miso,
			      MEMIPC_REQ_INIT,
			      0, NULL));
	return 0;
}

int isolation_connect_this_thread_remote(int cpu)
{
	struct memipc_thread_params *thread;
	pthread_t thread_id;
	long my_thread_pid;
	long my_thread_tid;
	struct tx_text tx;
	struct rx_buffer rx;
	int i, rcode_value;
	struct memipc_thread_params *threads;
	int threads_count;
	char one = 1;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;

	enum {
	      MANAGER_NEWTASK_KV_MODE,
	      MANAGER_NEWTASK_KV_INDEX,
	      MANAGER_NEWTASK_KV_CPU
	};
	enum {
	      MANAGER_MODE_THREAD,
	      MANAGER_MODE_PROCESS
	};
	char *kv_type_modes[]={"THREAD", "PROCESS", NULL};
	struct kv_rx kv[] = {
		{"MODE", KV_TYPE_ENUM, kv_type_modes, 0, .val.val_int = 0},
		{"INDEX", KV_TYPE_INT, NULL, 0, .val.val_int = 0},
		{"CPU", KV_TYPE_INT, NULL, 0, .val.val_ptr = NULL},
		{NULL}
	};

	thread_id = pthread_self();
	if (memipc_thread_self != NULL || memipc_thread_fd >= 0)
		return -EEXIST;
	tx_init(&tx);
	if (init_rx_buffer(&rx))
		return -ENOMEM;
#if USE_CPU_SUBSETS
	memipc_thread_fd = isol_client_connect_to_server(server_socket_name);
#else
	memipc_thread_fd = isol_client_connect_to_server(SERVER_SOCKET_NAME);
#endif
	if (memipc_thread_fd < 0) {
		free_rx_buffer(&rx);
		return -ENOENT;
	}
	my_thread_pid = (unsigned long) getpid();
	my_thread_tid = isolation_get_tid(thread_id);
	rcode_value = read_rx_data(&rx, memipc_thread_fd, NULL);
	if (rcode_value != 220) {
		close(memipc_thread_fd);
		memipc_thread_fd = -1;
		free_rx_buffer(&rx);
		return -EINVAL;
	}
	if (tx_add_text(&tx, "newtask ")
	    || tx_add_text_num(&tx, cpu)
	    || tx_add_text(&tx, ",")
	    || tx_add_text_num(&tx, my_thread_pid)
	    || tx_add_text(&tx, "/")
	    || tx_add_text_num(&tx, my_thread_tid)
	    || tx_add_text(&tx, "\n")
	    || send_tx_fd_persist(memipc_thread_fd, &tx)) {
		close(memipc_thread_fd);
		memipc_thread_fd = -1;
		free_rx_buffer(&rx);
		return -EINVAL;
	}

	rcode_value = read_rx_data(&rx, memipc_thread_fd, kv);
	if ((rcode_value != 200)
	    || !kv[MANAGER_NEWTASK_KV_MODE].set
	    || !kv[MANAGER_NEWTASK_KV_CPU].set) {
		close(memipc_thread_fd);
		memipc_thread_fd = -1;
		free_rx_buffer(&rx);
		return -EINVAL;
	}

	if (kv[MANAGER_NEWTASK_KV_MODE].val.val_int == MANAGER_MODE_THREAD) {
		if (!kv[MANAGER_NEWTASK_KV_INDEX].set) {
			close(memipc_thread_fd);
			memipc_thread_fd = -1;
			free_rx_buffer(&rx);
			return -EINVAL;
		}
		/* Thread mode */
		thread = &threads[kv[MANAGER_NEWTASK_KV_INDEX].val.val_int];
	} else {
		/* Process mode */
		thread = NULL;
		for (i = 0; i < threads_count; i++)
			if (threads[i].cpu ==
			    kv[MANAGER_NEWTASK_KV_CPU].val.val_int) {
				thread = &threads[i];
				i = threads_count;
			}
		if (thread == NULL) {
			/* FIXME -- allocate to support multiple threads
			   per process */
			thread = &threads[0];
			thread->cpu = kv[MANAGER_NEWTASK_KV_CPU].val.val_int;
		}
		thread->pid = my_thread_pid;
		thread->tid = my_thread_tid;
		/*
		  thread->isolated value 1 means
		  that initialization is in
		  progress, and thread may be not
		  in isolated state.
		*/
#ifdef DEBUG_LOG_ISOL_CHANGES
		write(1, "\nTHR_C_R, isolated = 1\n", 23);
#endif
		__atomic_store(&thread->isolated,
			       &one,
			       __ATOMIC_SEQ_CST);
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	/* Set per-thread environment and thread descriptor */
	memipc_my_pid = thread_id;
	memipc_thread_self = thread;
	thread->thread_id = thread_id;

	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	thread->s_memipc_mosi->reader = memipc_my_pid;
	thread->s_memipc_miso->writer = memipc_my_pid;
	thread->memipc_check_signal_ptr = &memipc_check_signal;
	thread->counter_ptr = NULL;
	memipc_check_signal = 0;

	/*
	  MEMIPC_REQ_INIT is no longer sent in this mode,
	  manager sets MEMIPC_STATE_STARTED state after
	  sending the response.
	*/
	free_rx_buffer(&rx);
	return 0;
}

/*
 * Send request to the manager to run this thread isolated.
 */
int isolation_request_launch_this_thread(volatile int *c)
{
	if (memipc_thread_self == NULL)
		return -1;

	memipc_thread_launch_confirmed = 0;
	memipc_thread_continue_flag = 1;
	/* Ready for isolation */
	while (memipc_add_req(memipc_thread_self->s_memipc_miso,
			      MEMIPC_REQ_START_READY,
			      c?sizeof(c):0, (unsigned char*)&c));

	/* Talk to the manager until entered isolation */
	do {
		memipc_thread_pass(memipc_thread_self);
	}
	while ((memipc_thread_launch_confirmed == 0)
	       && memipc_thread_continue_flag);
	/* Entered isolation or got request to terminate */

	if (!memipc_thread_continue_flag) {
		/* This thread is supposed to exit now, perform shutdown */
		prctl(PR_SET_TASK_ISOLATION, 0, 0, 0, 0);
#if ISOLATION_MONITOR_IN_SLAVE
		char zero = 0;
#ifdef DEBUG_LOG_ISOL_CHANGES
		write(1, "\nLAUNCHT, isolated = 0\n", 23);
#endif
		__atomic_store(&memipc_thread_self->isolated,
			       &zero,
			       __ATOMIC_SEQ_CST);
#endif
		while (memipc_add_req(memipc_thread_self->s_memipc_miso,
				      MEMIPC_REQ_EXITING,
				      0, NULL));
		if (memipc_thread_fd >= 0) {
			close(memipc_thread_fd);
			memipc_thread_fd = -1;
		}
	}
	return !memipc_thread_continue_flag;
}

/*
 * SIGUSR1 handler for thread isolation.
 */
static void isolation_sigusr1_handler(int sig)
{
	char zero = 0;
	int i;
	struct memipc_thread_params *threads;
	int threads_count;
	pthread_t thread_id;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;

	thread_id = pthread_self();
	for (i = 0; i < threads_count ; i++) {
		if (threads[i].thread_id == thread_id) {
#ifdef DEBUG_LOG_ISOL_CHANGES
			write(1, "\nSIGUSR1, isolated = 0\n", 23);
#endif
			__atomic_store(&threads[i].isolated,
				       &zero,
				       __ATOMIC_SEQ_CST);
#if ISOLATION_MONITOR_IN_SLAVE
			*threads[i].memipc_check_signal_ptr = 1;
#endif
			return;
		}
	}

	return;
}

/*
 * Compare unsigned integers for qsort().
 */
static int uintcmp(const void *v1, const void *v2)
{
	return (*((const unsigned int*)v1) >
		*((const unsigned int*)v2)) * 2 - 1;
}

/*
 * Allocate and fill a list of CPUs from a string.
 */
static int string_to_cpulist(const char *s, unsigned int **retbuf)
{
	const char *p;
	char *nextp;
	unsigned int *buf = NULL;
	int cpunum, last_cpu, last_oper, n_cpus, pass, i;
	for (pass = 0; pass < 2; pass++) {
		p = s;
		nextp = NULL;
		cpunum = 0;
		last_cpu = -1;
		last_oper = 0;
		n_cpus = 0;

		while (*p) {
			cpunum=strtol(p, (char ** restrict)&nextp, 0);
			if (nextp != p) {
				if (cpunum >= 0) {
					if ((last_oper == 1)
					    && (last_cpu >= 0)
					    && (cpunum > last_cpu)) {
						for (i = last_cpu + 1;
						     i <= cpunum; i++) {
							if (pass == 1)
								buf[n_cpus] =
								(unsigned int)i;
							n_cpus++;
						}
					} else {
						if (pass == 1)
							buf[n_cpus] =
							(unsigned int)cpunum;
						n_cpus++;
					}
					last_cpu = cpunum;
					last_oper = 0;
				}
			}
			if (*nextp) {
				if (*nextp == '-')
					last_oper = 1;
				nextp++;
			}
			p = nextp;
		}
		if (pass == 0) {
			if (n_cpus == 0)
				return -1;
			buf = (unsigned int *)malloc(n_cpus
						     * sizeof(unsigned int));
			if (buf == NULL)
				return -1;
		}
	}

	qsort(buf, n_cpus, sizeof(unsigned int), uintcmp);

	for (i = 0; i < n_cpus - 1; i++) {
		if (buf[i] == buf[i+1]) {
			if ((n_cpus - i) > 2)
				memmove(&buf[i + 1],
					&buf[i + 2],
					(n_cpus - i - 2)
					* sizeof(unsigned int));
			i--;
			n_cpus--;
		}
	}
	*retbuf = buf;
	return n_cpus;
}


/*
 * Initialize environment for a given CPU list.
 */
int memipc_isolation_initialize_cpulist(const char *cpulist)
{
	int n_cpus, i;
	unsigned int *buf = NULL;
#if USE_CPU_SUBSETS
	int j, subset_found, n_subset_cpus;
	unsigned int *subset_buf = NULL;
	FILE *f;
	char *p, *endp;
	char stringbuf[1024], *subset_id, *cpu_subset_str;
#endif
	struct memipc_thread_params *threads;

	if (_global_isolated_threads != NULL) {
		fprintf(stderr,
			"Isolation environment is already initialized\n");
		return -1;
	}

	n_cpus = string_to_cpulist(cpulist, &buf);
	if (n_cpus < 0)
		return -1;

	/* CPU set for everything but isolated threads */
	CPU_ZERO(&_global_nonisol_cpuset);
	sched_getaffinity(0, sizeof(cpu_set_t), &_global_nonisol_cpuset);

	for (i = 0; i < n_cpus; i++)
		CPU_CLR(buf[i], &_global_nonisol_cpuset);

#if USE_CPU_SUBSETS
	subset_found = 0;
	subset_id = getenv("CPU_SUBSET_ID");
	cpu_subset_str = getenv("CPU_SUBSET");
	if ((subset_id != NULL) && (cpu_subset_str != NULL))
		subset_found = 1;
	if ((subset_found == 0) && (subset_id != NULL)) {
		f = fopen(CPU_SUBSETS_FILE, "rt");
		if (f != NULL) {
			while ((subset_found == 0)
			       && (fgets(stringbuf,
					 sizeof(stringbuf), f) != NULL)) {
				p = strchr(stringbuf, '#');
				if (p != NULL)
					*p = '\0';
				p = strchr(stringbuf, ':');
				if (p != NULL) {
					*p = '\0';
					p++;
					skip_whitespace_nconst(&p);
					cpu_subset_str = p;
					p = stringbuf;
					skip_whitespace_nconst(&p);
					endp = find_endtoken_nconst(p);
					*endp = '\0';
					if (!strcmp(p, subset_id))
						subset_found = 1;
				}
			}
			fclose(f);
		}
	}
	if (subset_found) {
		n_subset_cpus = string_to_cpulist(cpu_subset_str, &subset_buf);
		if (n_subset_cpus < 0) {
			free(buf);
			return -1;
		}
		for (i = 0, j = 0; i < n_cpus; i++) {
			while ((j < n_subset_cpus) && (subset_buf[j] < buf[i]))
				j++;
			if ((j >= n_subset_cpus) || (subset_buf[j] != buf[i])) {
				memmove(&buf[i],
					&buf[i + 1],
					(n_cpus - i - 1)
					* sizeof(unsigned int));
				i--;
				n_cpus--;
			}
		}
		free(subset_buf);
	}
#endif
#if DEBUG_ISOL_VERBOSE
	fprintf(stderr, "Total %d CPUs capable of isolation:\n", n_cpus);
	for (i = 0; i < n_cpus; i++) {
		fprintf(stderr, "%d%s", buf[i],
			(i == (n_cpus - 1))?"\n":", ");
	}
#endif

	threads = (struct memipc_thread_params*)
		malloc(n_cpus * sizeof(struct memipc_thread_params));
	if (threads == NULL) {
		free(buf);
		return -1;
	}

	memset(threads, 0, n_cpus * sizeof(struct memipc_thread_params));

	/* CPU sets for isolation */
	CPU_ZERO(&_global_isol_cpuset);
	CPU_ZERO(&_global_running_cpuset);

	for (i = 0; i < n_cpus; i++) {
		threads[i].index = i;
		threads[i].cpu = buf[i];
		CPU_SET(threads[i].cpu, &_global_isol_cpuset);
		threads[i].memipc_name = memipc_area_name(threads[i].cpu);
		if (threads[i].memipc_name != NULL) {
			shm_unlink(threads[i].memipc_name);
			threads[i].memipc_fd =
				shm_open(threads[i].memipc_name,
					 O_RDWR | O_CREAT | O_TRUNC, 0600);
			if (threads[i].memipc_fd >= 0) {
				if (ftruncate(threads[i].memipc_fd,
					      AREA_SIZE * 2) < 0) {
					close(threads[i].memipc_fd);
					threads[i].memipc_fd = -1;
				}
			}
			if (threads[i].memipc_fd >= 0) {
				threads[i].m_memipc_mosi =
					memipc_area_create(AREA_SIZE,
							   AREA_SIZE * 2,
							   0,
							  threads[i].memipc_fd,
							   NULL);
				threads[i].m_memipc_miso =
					memipc_area_create(AREA_SIZE,
						0,
						AREA_SIZE,
						threads[i].memipc_fd,
						(unsigned char*)threads[i].
							   m_memipc_mosi->area);
				threads[i].s_memipc_mosi =
					memipc_area_dup(threads[i].
							m_memipc_mosi);
				threads[i].s_memipc_miso =
					memipc_area_dup(threads[i].
							m_memipc_miso);
			} else {
				threads[i].m_memipc_mosi = NULL;
				threads[i].m_memipc_miso = NULL;
				threads[i].s_memipc_mosi = NULL;
				threads[i].s_memipc_miso = NULL;
			}
		} else {
			threads[i].memipc_fd = -1;
			threads[i].m_memipc_mosi = NULL;
			threads[i].m_memipc_miso = NULL;
			threads[i].s_memipc_mosi = NULL;
			threads[i].s_memipc_miso = NULL;
		}
		threads[i].start_routine = NULL;
		threads[i].userdata = NULL;
		threads[i].foreign_desc = NULL;
		threads[i].lasttimer = KTIME_MAX;
		threads[i].updatetimer = KTIME_MAX;

		if ((threads[i].memipc_name == NULL)
		    || (threads[i].m_memipc_mosi == NULL)
		    || (threads[i].m_memipc_miso == NULL)
		    || (threads[i].s_memipc_mosi == NULL)
		    || (threads[i].s_memipc_miso == NULL)) {
			int j;
			for (j = 0; j <= i ; j++) {
				if (threads[j].m_memipc_mosi != NULL)
					memipc_area_delete(
						threads[j].m_memipc_mosi);
				if (threads[j].m_memipc_miso != NULL)
					memipc_area_delete(
						threads[j].m_memipc_miso);
				if (threads[j].s_memipc_mosi != NULL)
					memipc_area_delete_duplicate(
						threads[j].s_memipc_mosi);
				if (threads[j].s_memipc_miso != NULL)
					memipc_area_delete_duplicate(
						threads[j].s_memipc_miso);
				if (threads[i].memipc_fd < 0)
					close(threads[i].memipc_fd);
				if (threads[i].memipc_name != NULL) {
					shm_unlink(threads[i].memipc_name);
					free(threads[i].memipc_name);
				}
			}
			free(threads);
			free(buf);
			return -1;
		}
	}
	free(buf);

	_global_isolated_threads = threads;
	_global_isolated_thread_count = n_cpus;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	_global_isolated_threads_start_time = ts.tv_sec;
	_global_isolated_threads_timeout_started = 1;

	signal(SIGUSR1, isolation_sigusr1_handler);

	return 0;
}

/*
 * For all CPUs reset last timer to KTIME_MAX if it the last timer is
 * expired on that CPU.
 */
static void cpu_remove_expired_timers(int64_t now)
{
	int i;
	struct memipc_thread_params *threads;
	int threads_count;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;
	for (i = 0; i < threads_count; i++) {
		if ((threads[i].lasttimer != KTIME_MAX)
		    && ((threads[i].lasttimer - now) < 0)) {
			threads[i].lasttimer = KTIME_MAX;
			threads[i].updatetimer = now;
		}
	}
}

/*
 * Return time before expiration of the last timer on threads
 * that are being managed.
 */
static int64_t remaining_nsec_before_expiration(int64_t now)
{
	int i;
	struct memipc_thread_params *threads;
	int threads_count;
	int64_t remaining_last, remaining_current;
	int found_remaining;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;
	for (i = 0, found_remaining = 0, remaining_last = KTIME_MAX;
	     i < threads_count; i++) {
		if (((threads[i].state == MEMIPC_STATE_READY)
		     || (threads[i].state == MEMIPC_STATE_TMP_EXITING_ISOLATION)
		     || (threads[i].state == MEMIPC_STATE_EXITING_ISOLATION)
		     || (threads[i].state == MEMIPC_STATE_LOST_ISOLATION)
		     || (threads[i].state == MEMIPC_STATE_LAUNCHING)
		     || (threads[i].state == MEMIPC_STATE_LAUNCHED)
		     || (threads[i].state == MEMIPC_STATE_RUNNING)
		     )
		    && (threads[i].lasttimer != KTIME_MAX)
	    /*&& ((remaining_current = (threads[i].lasttimer - now)) >= 0)*/) {
			remaining_current = (threads[i].lasttimer - now);
			if (found_remaining) {
				if (remaining_current > remaining_last)
					remaining_last = remaining_current;
			} else {
				remaining_last = remaining_current;
				found_remaining = 1;
			}
		}
	}
	if (found_remaining)
		return remaining_last;
	else
		return KTIME_MAX;
}

/*
 * This function is supposed to update a timer for CPUs where threads may
 * run isolated. It returns 0 if given timer is not for CPUs in that set,
 * so it may be ignored later, and 1 if CPU is in this set.
 */
static int cpu_update_timer(enum isol_timer_type timer_type,
#if DEBUG_ISOL_NAMES
			    const char *addr, const char *handler,
#endif
			    int cpu, int64_t expire, int64_t now)
{
	int i;
	struct memipc_thread_params *threads;
	int threads_count;

	threads = _global_isolated_threads;
	threads_count = _global_isolated_thread_count;
	for (i = 0; i < threads_count; i++) {
		if (cpu == threads[i].cpu) {
			if (threads[i].lasttimer == KTIME_MAX)
				threads[i].lasttimer = expire;
			else {
				if ((threads[i].lasttimer - now) < 0)
					threads[i].lasttimer = KTIME_MAX;
				else
					if ((threads[i].lasttimer - expire) < 0)
						threads[i].lasttimer = expire;
			}
			threads[i].updatetimer = now;
#if DEBUG_ISOL_ALWAYS_SHOW_ALL_TIMERS
#if DEBUG_ISOL_NAMES
			fprintf(stderr, "Timer on CPU %d, type %s, "
			"at %s, handler %s, expires at %lu in %ld nsec\n",
				cpu, isol_timer_type_name[timer_type],
				addr, handler, (uint64_t)expire, expire - now);
#else
			fprintf(stderr, "Timer on CPU %d, type %s, "
				"expires at %lu in %ld nsec\n",
				cpu, isol_timer_type_name[timer_type],
				(uint64_t)expire, expire - now);
#endif
#endif
			return 1;
		}
	}
	return 0;
}

static int client_show_banner(int client_index)
{
	const char *banner =
		"220-Task Manager.\n"
		"220 Session started.\n";
	send_data_persist(client_index, banner, strlen(banner));
	return 0;
}


unsigned int get_uint(const char *s)
{
	unsigned int val;
	for (val = 0; (*s >= '0') && (*s <= '9'); s++) {
		val *= 10;
		val += *s - '0';
	}
	return val;
}

int get_int(const char *s)
{
	int val, sign;

	if (*s == '-') {
		sign = -1;
		s++;
	} else
		sign = 1;

	for (val = 0; (*s >= '0') && (*s <= '9'); s++) {
		val *= 10;
		val += *s - '0';
	}
	return val * sign;
}

static int client_text_handler(int client_index, const char *line)
{
	enum {
	      ISOL_SRV_CMD_QUIT,
	      ISOL_SRV_CMD_TERMINATE,
	      ISOL_SRV_CMD_NEWTASK,
	      ISOL_SRV_CMD_TASKISOLFAIL,
	      ISOL_SRV_CMD_TASKISOLFINISH,
	      ISOL_SRV_CMD_ARRAY_SIZE
	};

	const char *commands[ISOL_SRV_CMD_ARRAY_SIZE] =	{
	      "quit",
	      "terminate",
	      "newtask",
	      "taskisolfail",
	      "taskisolfinish"
	};

	int command_len[ISOL_SRV_CMD_ARRAY_SIZE] = {
	      4,
	      9,
	      7,
	      12,
	      14
	};

	const char *p, *p1, *p2, *arg,
		*inv_response =
		"500 Invalid command.\n",
		*alr_connected_response =
		"500 Already connected.\n",
		*no_task_resp =
		"500 No task connected.\n",
		*cant_alloc_resp =
		"500 Can't allocate CPU.\n",
		*endsession =
		"221 End of session.\n",
		*terminating =
		"200-Terminating threads.\n"
		"221 End of session.\n",
		*ok_resp =
		"220 Ok\n";

	int i, command, client_cpu;
	pid_t client_pid, client_tid;
	struct tx_text serv_resp;
	struct memipc_thread_params *thread;
	char one = 1;

	for (p = line; ((unsigned char)*p) > ' '; p++);

	for (command = ISOL_SRV_CMD_ARRAY_SIZE, i = 0;
	     i < ISOL_SRV_CMD_ARRAY_SIZE; i++) {
		if (((p - line) == command_len[i])
		    && !memcmp(line, commands[i], command_len[i])) {
			command = i;
			i = ISOL_SRV_CMD_ARRAY_SIZE;
		}
	}
	while (*p && (unsigned char)*p <= ' ')
		p++;
	if (*p)
		arg = p;
	else
		arg = NULL;

	switch (command) {
	case ISOL_SRV_CMD_QUIT:
		send_data_persist(client_index, endsession,
				  strlen(endsession));
		close_client_connection(client_index);
		break;
	case ISOL_SRV_CMD_TERMINATE:
		send_data_persist(client_index, terminating,
				  strlen(terminating));
		memipc_isolation_terminate_all_threads();
		close_client_connection(client_index);
		break;
	case ISOL_SRV_CMD_NEWTASK:
		/* pid and tid 0 are invalid, initialize to those values */
		client_pid = 0;
		client_tid = 0;
		client_cpu = -1;
		if (arg == NULL) {
			send_data_persist(client_index, inv_response,
					  strlen(inv_response));
		} else {
			/* Parse PID/TID argument */
			p1 = strchr(arg, ',');
			if (p1 == NULL) {
				send_data_persist(client_index,
						  inv_response,
						  strlen(inv_response));
			} else {
				client_cpu = get_int(arg);
				p2 = strchr(p1, '/');
				if (p2 == NULL) {
					send_data_persist(client_index,
							  inv_response,
							  strlen(inv_response));
				} else {
					p1++;
					p2++;
					client_pid = get_uint(p1);
					client_tid = get_uint(p2);
				}
			}
		}
		if ((client_pid != 0) && (client_tid != 0)) {
			/* Check if this client already has a task attached */
			if (get_client_task(client_index) != NULL) {
				send_data_persist(client_index,
						  alr_connected_response,
						strlen(alr_connected_response));
			} else {
				/* Start a new task, mark client as this task */
				thread = isolation_claim_cpu(client_cpu);
				if (thread == NULL) {
					send_data_persist(client_index,
							  cant_alloc_resp,
						  strlen(cant_alloc_resp));
				} else {
					/*
					  Zero all data that should be used
					  only by the client
					*/
					thread->memipc_check_signal_ptr = NULL;
					thread->counter_ptr = NULL;
					thread->thread_id = 0;
					thread->pid = client_pid;
					thread->tid = client_tid;
					/*
					  thread->isolated value 1 means
					  that initialization is in
					  progress, and thread may be not
					  in isolated state.
					*/
#ifdef DEBUG_LOG_ISOL_CHANGES
					write(1, "\nTXTHAND, isolated = 1\n",
					      23);
#endif
					__atomic_store(&thread->isolated,
						       &one,
						       __ATOMIC_SEQ_CST);
					/* Prepare and send response */
					tx_init(&serv_resp);
					tx_add_text(&serv_resp,
						    "200-Task allocated\n");
					if (thread->pid == getpid()) {
						tx_add_text(&serv_resp,
							    "200-MODE=THREAD\n"
							    "200-INDEX=");
						tx_add_text_num(&serv_resp,
								thread->index);
					} else {
						tx_add_text(&serv_resp,
							"200-MODE=PROCESS\n");
					}
					tx_add_text(&serv_resp,
						    "\n200-CPU=");
					tx_add_text_num(&serv_resp,
							thread->cpu);
					tx_add_text(&serv_resp,
						    "\n200 OK\n");
					send_tx_persist(client_index,
							&serv_resp);
					/* Associate a task with a client */
					set_client_task(client_index,
							(void*)thread);
					/*
					  The following was done in
					  response to MEMIPC_REQ_INIT,
					  that is no longer used in this
					  mode
					*/
					CPU_SET(thread->cpu,
						&_global_running_cpuset);
					thread->state = MEMIPC_STATE_STARTED;
				}
			}
		}
		break;
	case ISOL_SRV_CMD_TASKISOLFAIL:
		/* This is an equivalent of MEMIPC_REQ_START_LAUNCH_FAILURE
		   message */
		/* Check if this client has a task attached */
		thread = (struct memipc_thread_params *)
			get_client_task(client_index);
		if (thread == NULL) {
			send_data_persist(client_index,
					  no_task_resp,
					  strlen(no_task_resp));
		} else {
			/* Should react to the launch failure */
#if DEBUG_ISOL_VERBOSE
			fprintf(stderr,
				"Socket message: Isolation failure on CPU %d\n",
				thread->cpu);
#endif
			/* Re-launch */
#if ISOLATION_MONITOR_IN_MASTER
		{
			char isolated_state;
			__atomic_load(&thread->isolated,
				      &isolated_state,
				      __ATOMIC_SEQ_CST);
			if (isolated_state == 0) {
				if ((thread->state !=
				     MEMIPC_STATE_TMP_EXITING_ISOLATION)
				    && (thread->state !=
					MEMIPC_STATE_EXITING_ISOLATION)) {
					thread->state =
						MEMIPC_STATE_LOST_ISOLATION;
					clock_gettime(CLOCK_MONOTONIC,
						      &thread->isol_exit_time);
					if (memipc_add_req(
						   thread->m_memipc_mosi,
						   MEMIPC_REQ_START_LAUNCH,
						   0, NULL) == 0) {
						if (thread->counter_ptr != NULL)
						(*(thread->counter_ptr))++;
						thread->state =
							MEMIPC_STATE_LAUNCHING;
#if DEBUG_ISOL_VERBOSE
						fprintf(stderr,
					     "Re-launching thread on CPU %d\n",
							thread->cpu);
#endif
					}
				}
			}
		}
#else
		if ((thread->state != MEMIPC_STATE_TMP_EXITING_ISOLATION)
		    && (thread->state != MEMIPC_STATE_EXITING_ISOLATION)) {
			thread->state = MEMIPC_STATE_LOST_ISOLATION;
			clock_gettime(CLOCK_MONOTONIC, &thread->isol_exit_time);
			if (memipc_add_req(thread->m_memipc_mosi,
					   MEMIPC_REQ_START_LAUNCH,
					   0, NULL) == 0) {
				if (thread->counter_ptr != NULL)
					(*(thread->counter_ptr))++;
				thread->state = MEMIPC_STATE_LAUNCHING;
#if DEBUG_ISOL_VERBOSE
				fprintf(stderr,
					"Re-launching thread on CPU %d\n",
					thread->cpu);
#endif
			}
		}
#endif
		send_data_persist(client_index,
				  ok_resp,
				  strlen(ok_resp));
		}
		break;
	case ISOL_SRV_CMD_TASKISOLFINISH:
		/* This is an equivalent of MEMIPC_REQ_EXITING message */
		/* Check if this client has a task attached */
		thread = (struct memipc_thread_params *)
			get_client_task(client_index);
		if (thread == NULL) {
			send_data_persist(client_index,
					  no_task_resp,
					  strlen(no_task_resp));
		} else {
			/* Finish, optionally join exiting thread */
			thread->state = MEMIPC_STATE_OFF;
			thread->counter_ptr = NULL;
			thread->exit_request = 0;
			CPU_CLR(thread->cpu, &_global_running_cpuset);
			if (thread->foreign_desc != NULL)
				memipc_detach_thread_from_desc(
						       thread->foreign_desc);
			if (thread->pid == getpid()) {
				/* Thread from the same process, join it. */
				pthread_join(thread->thread_id, NULL);
			}
#if ISOLATION_MONITOR_IN_MASTER
#ifdef DEBUG_LOG_ISOL_CHANGES
			write(1, "\nISOLFIN, isolated = 0\n", 23);
#endif
			__atomic_store_n(&thread->isolated,
					 0,
					 __ATOMIC_SEQ_CST);
#endif
			thread->start_routine = NULL;
			thread->userdata = NULL;
			thread->lasttimer = KTIME_MAX;
			thread->updatetimer = KTIME_MAX;
			isolation_release_cpu(thread);
			/* Disconnect a task from a client. */
			set_client_task(client_index, NULL);
			/* End client session. */
			send_data_persist(client_index, endsession,
					  strlen(endsession));
			close_client_connection(client_index);
#if DEBUG_ISOL_VERBOSE
			fprintf(stderr, "Thread on CPU %d exited\n",
				thread->cpu);
#endif
		}
		break;
	default:
		send_data_persist(client_index, inv_response,
				  strlen(inv_response));
		break;
	}
	return 0;
}

static int client_disconnect_handler(int client_index)
{
	struct memipc_thread_params *thread;
	/* This is an equivalent of MEMIPC_REQ_EXITING message */
	/* Check if this client has a task attached */
	thread = (struct memipc_thread_params *)
		get_client_task(client_index);
	if (thread != NULL) {
		/* Finish, optionally join exiting thread */
		thread->state = MEMIPC_STATE_OFF;
		thread->exit_request = 0;
		CPU_CLR(thread->cpu, &_global_running_cpuset);
		if (thread->foreign_desc != NULL)
			memipc_detach_thread_from_desc(thread->foreign_desc);
		if (thread->pid == getpid()) {
			/* Thread from the same process, join it. */
			pthread_join(thread->thread_id, NULL);
		}
#if ISOLATION_MONITOR_IN_MASTER
#ifdef DEBUG_LOG_ISOL_CHANGES
		write(1, "\nDISCONN, isolated = 0\n", 23);
#endif
		__atomic_store_n(&thread->isolated,
				 0,
				 __ATOMIC_SEQ_CST);
#endif
		thread->start_routine = NULL;
		thread->userdata = NULL;
		thread->lasttimer = KTIME_MAX;
		thread->updatetimer = KTIME_MAX;
		isolation_release_cpu(thread);
		/* Disconnect a task from a client. */
		set_client_task(client_index, NULL);
		/* End client session. */
		close_client_connection(client_index);
#if DEBUG_ISOL_VERBOSE
		fprintf(stderr, "Thread on CPU %d exited\n", thread->cpu);
#endif
	}
	return 0;
}

/*
 * Initialize environment for all CPUs available for task isolation.
 */
int memipc_isolation_initialize(void)
{
	char stringbuf[1024];
	FILE *f;
	int rv, fd, lockfd;
#if USE_CPU_SUBSETS
	char *subset_id, *server_socket_lock_name;
	int subset_id_len;

	subset_id = getenv("CPU_SUBSET_ID");
	if (server_socket_name != NULL)
		free(server_socket_name);
	if (subset_id != NULL) {
		subset_id_len = strlen(subset_id);
		server_socket_name = (char *)
			malloc(sizeof(SERVER_SOCKET_NAME) + 1
			       + subset_id_len);
		if (server_socket_name == NULL)
			return -1;
		server_socket_lock_name = (char *)
			malloc(sizeof(SERVER_SOCKET_NAME) + 1 + 4
			       + subset_id_len);
		if (server_socket_lock_name == NULL) {
			free(server_socket_name);
			server_socket_name = NULL;
			return -1;
		}
		strcpy(server_socket_name, SERVER_SOCKET_NAME);
		server_socket_name[sizeof(SERVER_SOCKET_NAME) - 1] = '.';
		strcpy(server_socket_name
		       + sizeof(SERVER_SOCKET_NAME),
		       subset_id);
		strcpy(server_socket_lock_name, server_socket_name);
		strcpy(server_socket_lock_name
		       + sizeof(SERVER_SOCKET_NAME)
		       + subset_id_len,
		       ".LCK");
	} else {
		server_socket_name = (char *)
			malloc(sizeof(SERVER_SOCKET_NAME));
		if (server_socket_name == NULL)
			return -1;
		server_socket_lock_name = (char *)
			malloc(sizeof(SERVER_SOCKET_NAME) + 4);
		if (server_socket_lock_name == NULL) {
			free(server_socket_name);
			server_socket_name = NULL;
			return -1;
		}
		strcpy(server_socket_name, SERVER_SOCKET_NAME);
		strcpy(server_socket_lock_name, server_socket_name);
		strcpy(server_socket_lock_name + sizeof(SERVER_SOCKET_NAME) - 1,
		       ".LCK");
	}
#endif
	f = fopen("/sys/devices/system/cpu/task_isolation", "rt");
	if (f == NULL)
		f = fopen("/sys/devices/system/cpu/isolated", "rt");
	if (f == NULL) {
		rv = -1;
		goto finish;
	}
	if (fgets(stringbuf, sizeof(stringbuf), f) == NULL) {
		fclose(f);
		rv = -1;
		goto finish;
	}
	fclose(f);
	rv = memipc_isolation_initialize_cpulist(stringbuf);
	if (rv == 0) {
		signal(SIGPIPE, SIG_IGN);
		set_client_line_handler(client_text_handler);
		set_client_connect_handler(client_show_banner);
		set_client_disconnect_handler(client_disconnect_handler);
		/* Create or open the lock file. */
#if USE_CPU_SUBSETS
		lockfd = open(server_socket_lock_name,
			      O_CREAT|O_RDONLY, 0600);
#else
		lockfd = open(SERVER_SOCKET_NAME ".LCK",
			      O_CREAT|O_RDONLY, 0600);
#endif
		if (lockfd < 0) {
#if DEBUG_ISOL_VERBOSE
			perror("Can't open the lock file");
#endif
			rv = -1;
			goto finish;
		}
		/* Lock */
		if (flock(lockfd, LOCK_EX) != 0) {
#if DEBUG_ISOL_VERBOSE
			perror("Can't lock file");
#endif
			close(lockfd);
			rv = -1;
			goto finish;
		}
#if USE_CPU_SUBSETS
		rv = isol_server_socket_create(server_socket_name);
#else
		rv = isol_server_socket_create(SERVER_SOCKET_NAME);
#endif
		if (rv != 0) {
			/*
			  The following mechanism will try to determine if
			  there is a stale socket file left from a
			  previous process. In the absence of locking it
			  would reliably detect such a stale socket unless
			  there is another process starting at the same
			  time. If there is another process starting, it
			  may determine that the socket is stale, delete
			  it, and create a new one, however another
			  process may do exactly the same at the same
			  time, so one of the newly created sockets will
			  be deleted. Locking prevents this scenario.
			*/
#if DEBUG_ISOL_VERBOSE
			fprintf(stderr,
				"Can't create socket, checking "
				"if the process is running\n");
#endif
#if USE_CPU_SUBSETS
			fd = isol_client_connect_to_server(server_socket_name);
#else
			fd = isol_client_connect_to_server(SERVER_SOCKET_NAME);
#endif
			if (fd < 0) {
#if DEBUG_ISOL_VERBOSE
				fprintf(stderr,
					"Process is not running, "
					"removing stale socket and "
					"creating a new one\n");
#endif
#if USE_CPU_SUBSETS
				unlink(server_socket_name);
				rv = isol_server_socket_create(
							server_socket_name);
#else
				unlink(SERVER_SOCKET_NAME);
				rv = isol_server_socket_create(
							SERVER_SOCKET_NAME);
#endif
			} else {
#if DEBUG_ISOL_VERBOSE
				fprintf(stderr, "Process is already running\n");
#endif
				close(fd);
			}
		}
		/* Unlock and close. */
		close(lockfd);
	}
 finish:
#if USE_CPU_SUBSETS
	if (rv != 0) {
		free(server_socket_name);
		server_socket_name = NULL;
	}
	free(server_socket_lock_name);
#endif
	return rv;
}

/* Wrappers for TMC */

/*
 * Initialize the configuration in the initial thread
 */
int tmc_isol_init(void)
{
	return memipc_isolation_initialize();
}


/*
 * Start
 */
int tmc_isol_start(void)
{
	return memipc_isolation_run_threads();
}

/*
 * Initialize thread's connection to the isolation mechanism
 *
 * This version selects CPU automatically if it is not set already.
 */
int tmc_isol_thr_init(void)
{
	int i, cpu;
	cpu_set_t cpuset;
	pthread_t thread_id;
	thread_id = pthread_self();

	if (pthread_getaffinity_np(thread_id, sizeof(cpu_set_t),
				   &cpuset) == 0) {
		for (i = 0, cpu = -1; (cpu < 0) && (i < CPU_SETSIZE); i++) {
			if (CPU_ISSET(i, &cpuset))
				cpu = i;
		}
		for (; (cpu >= 0) && (i < CPU_SETSIZE); i++) {
			if (CPU_ISSET(i, &cpuset))
				cpu = -1;
		}
	} else
		cpu = -1;

	if (isolation_connect_this_thread_remote(cpu))
		return isolation_connect_this_thread_remote(-1);
	else
		return 0;
}

/*
 * Enter isolation mode while connected to the manager.
 */
int tmc_isol_thr_enter_v(volatile int *c)
{
	return isolation_request_launch_this_thread(c);
}

/*
 * Exit isolation mode while connected to the manager
 */
int tmc_isol_thr_exit(void)
{
	if (memipc_thread_self == NULL)
		return -1;
	memipc_isolation_request_leave_isolation();

	prctl(PR_SET_TASK_ISOLATION, 0, 0, 0, 0);
#if ISOLATION_MONITOR_IN_SLAVE
	char zero = 0;
#ifdef DEBUG_LOG_ISOL_CHANGES
	write(1, "\nTHR_EXI, isolated = 0\n", 23);
#endif
	__atomic_store(&memipc_thread_self->isolated,
		       &zero,
		       __ATOMIC_SEQ_CST);
#endif
	memipc_isolation_announce_exit();

	if (memipc_thread_fd >= 0) {
		close(memipc_thread_fd);
		memipc_thread_fd = -1;
	}
	return 0;
}


/*
 * Thread pass function, returns nonzero if exit is requested.
 */
int _tmc_isol_thr_pass(void)
{
	return memipc_thread_pass_default();
}

int tmc_printf(const char *fmt, ...)
{
	return memipc_isolation_printf(fmt);
}

/*
  Example thread functions.
 */
#if 0
/*
  This function is for threads that are created managed. It runs in
  isolated environment, and only has to call
  memipc_thread_pass_default() to perform monitoring and control.
*/
static void *default_thread_handler_1(void *arg_data)
{
	unsigned counter = 1;
	unsigned writefailcounter = 0;

	memipc_isolation_printf("Thread is running\n");
	/* loop until exit is requested */
	while (memipc_thread_pass_default()) {
		if (memipc_isolation_printf("Test thread output, "
					    "Message number %u, "
					    "could not write %u times\n",
					    counter, writefailcounter) >= 0) {
			writefailcounter = 0;
			counter++;
		} else
			writefailcounter++;
	}
	return NULL;
}

/*
  This function is for threads that are created as regular threads.
  It has to initialize isolated environment for the thread by calling
  tmc_isol_thr_init(), then call tmc_isol_thr_enter() to enter
  isolated environment. It still has to call
  memipc_thread_pass_default() to perform monitoring and control.
*/
static void *default_thread_handler_2(void *arg_data)
{
	unsigned counter = 1;
	unsigned writefailcounter = 0;

	if (tmc_isol_thr_init())
		return NULL;

	if (tmc_isol_thr_enter())
		return NULL;

	/* loop until exit is requested */
	while (memipc_thread_pass_default()) {
		if (memipc_isolation_printf("Test thread output, "
					    "Message number %u, "
					    "could not write %u times\n",
					    counter, writefailcounter) >= 0) {
			writefailcounter = 0;
			counter++;
		} else
			writefailcounter++;
	}
	/* Exit from isolation */
	tmc_isol_thr_exit();
	return NULL;
}
#endif
