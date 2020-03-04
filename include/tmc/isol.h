#ifndef __TMC_ISOL_H__
#define __TMC_ISOL_H__

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

#include <stdint.h>
#include <stddef.h>

extern __thread volatile unsigned char *memipc_check_newdata_ptr;
extern __thread volatile unsigned char memipc_check_signal;
extern __thread int memipc_thread_continue_flag;

int tmc_isol_init(void);
int tmc_isol_start(void);
int tmc_isol_thr_init(void);
int tmc_isol_thr_enter_v(volatile int *c);
int tmc_isol_thr_exit(void);
int _tmc_isol_thr_pass(void);

#if ISOLATION_MONITOR_IN_SLAVE
#define TMC_ISOL_THR_PASS_MIN_CHECK(x, c1, c2)				\
  ((((--x) || ((c1 == c2) ? 1 : (c1++, 0)))				\
    && ((memipc_check_signal & 1) == 0))				\
   || ((((*memipc_check_newdata_ptr) | memipc_check_signal) & 1) ?	\
       _tmc_isol_thr_pass() : memipc_thread_continue_flag))
#else
#define TMC_ISOL_THR_PASS_MIN_CHECK(x, c1, c2)				\
  ((--x)								\
   || (c1 == c2)							\
   || (c1++, ((*memipc_check_newdata_ptr) & 1) ?			\
       _tmc_isol_thr_pass() : memipc_thread_continue_flag))
#endif

#if ISOLATION_MONITOR_IN_SLAVE
#define TMC_ISOL_THR_PASS(c1, c2)					\
  (__builtin_expect(							\
		    (((__builtin_expect((c1 == c2), 1) ? 1 : (c1++, 0))	\
		      && __builtin_expect(((memipc_check_signal & 1)	\
					   == 0), 1))			\
		     || ((((*memipc_check_newdata_ptr) |		\
			   memipc_check_signal) & 1) ?			\
			 _tmc_isol_thr_pass()				\
			 : memipc_thread_continue_flag)), 1))
#else
#define TMC_ISOL_THR_PASS(c1, c2)					\
  (__builtin_expect(							\
		    (__builtin_expect((c1 == c2), 1)			\
		     || (c1++, ((*memipc_check_newdata_ptr) & 1) ?	\
			 (_tmc_isol_thr_pass()) :			\
			 memipc_thread_continue_flag)), 1))
#endif

static inline int tmc_isol_thr_enter(void) {
	return tmc_isol_thr_enter_v((volatile int *)NULL);
}

static inline int tmc_isol_thr_pass(void) {
	return (((*memipc_check_newdata_ptr)
#if ISOLATION_MONITOR_IN_SLAVE
		 | memipc_check_signal
#endif
		 ) & 1) ?
		_tmc_isol_thr_pass() : memipc_thread_continue_flag;
}

int tmc_printf(const char *fmt, ...);

#endif /* __TMC_ISOL_H__ */
