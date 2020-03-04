#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include <tmc/isol.h>
#include "isol-internals.h"
#pragma GCC diagnostic ignored "-Wunused-parameter"

#ifndef CREATE_THREADS_MANAGED
#define CREATE_THREADS_MANAGED 0
#endif

#if CREATE_THREADS_MANAGED
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
	while (tmc_isol_thr_pass()) {
		if (memipc_isolation_printf("Test thread output, "
					    "* Message number %u, "
					    "could not write %u times\n",
					    counter, writefailcounter) >= 0) {
			writefailcounter = 0;
			counter++;
		} else
			writefailcounter++;
	}
	return NULL;
}
#else
/*
  This function is for threads that are created as regular threads.
  It has to initialize isolated environment for the thread by calling
  tmc_isol_thr_init(), then call tmc_isol_thr_enter() to enter
  isolated environment. It still has to call
  memipc_thread_pass_default() to perform monitoring and control.
*/
unsigned long long countlimit = 0;
static void *default_thread_handler_2(void *arg_data)
{
	unsigned counter = 1;
	unsigned long long passcounter = 0;
	unsigned writefailcounter = 0;

	int c1 = 0;
	volatile int c2 = 0;

	if (tmc_isol_thr_init())
		return NULL;

	if (tmc_isol_thr_enter_v(&c2))
		return NULL;

	/* loop until exit is requested */
	if (countlimit) {
		while (TMC_ISOL_THR_PASS(c1, c2)) {
			passcounter++;
			if (__builtin_expect((passcounter % 1000000000 == 0),
					     0)) {
				if (memipc_isolation_printf(
						"Test thread output, "
						"pass %lu, message number %u\n",
							    passcounter,
							    counter) >= 0) {
					writefailcounter = 0;
					counter++;
				} else
					writefailcounter++;
				if (__builtin_expect((passcounter
						      >= countlimit), 0)) {
					/* Exit from isolation */
					tmc_isol_thr_exit();
					return NULL;
				}
			}
		}
	} else {
		while (TMC_ISOL_THR_PASS(c1, c2)) {
			passcounter++;
			if (__builtin_expect((passcounter % 1000000000 == 0),
					     0)) {
				if (memipc_isolation_printf(
						"Test thread output, "
						"pass %lu, message number %u\n",
							    passcounter,
							    counter) >= 0) {
					writefailcounter = 0;
					counter++;
				} else
					writefailcounter++;
			}
		}
	}
	/* Exit from isolation */
	tmc_isol_thr_exit();
	return NULL;
}
#endif

int main(int argc, char **argv)
{
	int threads_count;
	int i;
	int opt;
	unsigned long long val;

	while ((opt = getopt(argc, argv, "c:")) != -1) {
		switch (opt) {
		case 'c':
			if (sscanf(optarg, "%llu", &val) == 1) {
				countlimit = val;
			} else {
				fprintf(stderr,
					"Invalid count: $s\n", optarg);
				return 1;
			}
			break;
		default:
			fprintf(stderr,
				"Usage: %s [-c <count>]\n",
				argv[0]);
			return 1;
		}
	}
	
	if (tmc_isol_init()) {
		fprintf(stderr, "Isolation initialization failed\n");
		return 1;
	}

	/* This is just used to count the available CPUs */
	threads_count = memipc_isolation_get_max_isolated_threads_count();

	for (i = 0; i < threads_count; i++) {
#if CREATE_THREADS_MANAGED
		if (isolation_thread_create(-1,
					    NULL,
					    NULL,
					    default_thread_handler_1,
					    NULL))
			fprintf(stderr, "Thread #%d failed to start\n", i);
#else
		pthread_t curr_thread_id;
		if (pthread_create(&curr_thread_id, NULL,
				   default_thread_handler_2, NULL))
			fprintf(stderr, "Thread #%d failed to start\n", i);
#endif
	}
	return tmc_isol_start();
}
