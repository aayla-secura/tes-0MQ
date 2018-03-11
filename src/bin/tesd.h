/*
 * Common to tasks and coordinator.
 */

#ifndef __TESD_H__INCLUDED__
#define __TESD_H__INCLUDED__

/* CPU_SET and friends */
#ifdef linux
#  define _GNU_SOURCE
#  include <pthread.h>
#else
#  include <pthread_np.h>
#endif

#include <sys/types.h>
#ifdef linux
#  include <sched.h>
#  define cpuset_t cpu_set_t
#else
#  include <sys/_cpuset.h>
#  include <sys/cpuset.h>
#endif
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>

// #define CZMQ_BUILD_DRAFT_API
#include <czmq.h>

/* Debug levels. */
#define NO_DEBUG      0
#define CAUTIOUS      1
#define TESTING       5
#define VERBOSE      10
#define ARE_YOU_NUTS 50 // expect output every ~1 packet
#define DEBUG_LEVEL TESTING

#if DEBUG_LEVEL > NO_DEBUG
#  define TESPKT_DEBUG
#  define dbg_assert(...) assert (__VA_ARGS__)
#else
#  define dbg_assert(...)
#endif

#ifndef PATH_MAX
#  ifdef MAXPATHLEN
#    define PATH_MAX MAXPATHLEN
#  else
#    define PATH_MAX 4096
#  endif
#endif

#ifndef NUM_RINGS
#define NUM_RINGS 4 /* number of rx rings in interface */
#endif

#include "api.h"
#include "daemon_ng.h"
#include "ansicolors.h"

#endif
