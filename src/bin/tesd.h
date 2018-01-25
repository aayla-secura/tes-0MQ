#ifndef __TESD_H__INCLUDED__
#define __TESD_H__INCLUDED__

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <assert.h>

// #define CZMQ_BUILD_DRAFT_API
#include <czmq.h>

#include "daemon_ng.h"

#define ENABLE_DEBUG
#define ENABLE_FULL_DEBUG

#ifdef ENABLE_FULL_DEBUG
#  ifndef ENABLE_DEBUG
#    define ENABLE_DEBUG
#  endif
#  define TESPKT_DEBUG
#endif

#ifdef ENABLE_DEBUG
#  define dbg_assert(...) assert (__VA_ARGS__)
#else
#  define dbg_assert(...)
#endif

#define TES_MCASIZE_BUG /* FIX: overflow bug, last_bin is too large */

#include "net/tespkt.h"

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

#endif
