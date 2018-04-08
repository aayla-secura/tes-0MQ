/*
 * Common to tasks and coordinator.
 */

#ifndef __TESD_H__INCLUDED__
#define __TESD_H__INCLUDED__

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdbool.h>
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
#define NO_DEBUG        0
#define CAUTIOUS        1
#define TESTING         5
#define VERBOSE        10
#define FEELING_LUCKY  30
#define LETS_GET_NUTS  50 // Have you ever debugged with the devil
                          // by the pale moon light?
#ifndef DEBUG_LEVEL
#  define DEBUG_LEVEL TESTING
#endif

#if DEBUG_LEVEL > NO_DEBUG
#  define TESPKT_DEBUG
#  define dbg_assert(...) assert (__VA_ARGS__)
#else
#  define dbg_assert(...)
#endif

#ifndef NUM_RINGS
#define NUM_RINGS 4 /* number of rx rings in interface */
#endif

#include "api.h"
#include "daemon_ng.h"
#include "cutil.h"
#include "ansicolors.h"

#endif
