#ifndef __COMMON_H__INCLUDED__
#define __COMMON_H__INCLUDED__

#include <sys/types.h>
// #include <sys/time.h>
// #include <sys/mman.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <syslog.h>
#include <string.h>
// #include <signal.h>
// #include <poll.h>
// #include <pthread.h>

// #define CZMQ_BUILD_DRAFT_API
#include <czmq.h>

#define FPGAPKT_DEBUG
#include <net/fpgapkt.h>

/* Signals for communicating between coordinator and task threads */
#define SIG_INIT   0 /* task -> coordinator thread when ready */
#define SIG_STOP   1 /* coordinator -> task when error or shutting down */
#define SIG_DIED   2 /* task -> coordinator when error */
#define SIG_WAKEUP 3 /* coordinator -> task when new packets arrive */

/*
 * The verbosity and the daemon state will eventually be taken from
 * command-line options passed when the server is started. They'll then be
 * saved in external variables (probably), and the methods for printing
 * messages will use the daemon's state.
 */

// #define FULL_DBG    // will use assert throughout
#define VERBOSE     // will print debugging messages
#define MULTITHREAD // will print thread id in debug messages
// #define BE_DAEMON  // will go into background

/* ------------------------------------------------------------------------- */

// #ifdef FULL_DBG
// #  define DBG_ASSERT(..) assert (__VA_ARGS__)
// #else
// #  define DBG_ASSERT(..)
// #endif

/* ------------------------------------------------------------------------- */

/* When not using syslog and being verbose, debugging output will go to stderr,
 * and errors will go to stdout */
#define DBG_STREAM stderr

#ifdef VERBOSE
#  define ERR_STREAM stdout
#else
#  define ERR_STREAM stderr
#endif

/* The '%s' following msg in the PRIV versions corresponds to the empty string
 * passed by the non PRIV version. It is done in order to handle the case of
 * only one argument (no format specifiers) */

/* ------------------------------------------------------------------------- */
#ifdef SYSLOG

#define ERROR_PRIV(msg, ...) if (1) { \
	if (errno) \
		syslog (LOG_ERR, msg"%s: %m", __VA_ARGS__); \
	else \
		syslog (LOG_ERR, msg"%s", __VA_ARGS__); \
	} else (void)0

#define WARN_PRIV(msg, ...) \
	syslog (LOG_WARN, msg"%s", __VA_ARGS__)
#define INFO_PRIV(msg, ...) \
	syslog (LOG_WARN, msg"%s", __VA_ARGS__)

#ifdef MULTITHREAD
#  define DEBUG_PRIV(msg, ...) \
	syslog(LOG_DEBUG, "Thread %p: "msg"%s", (void*)pthread_self(), \
		__VA_ARGS__)
#else /* MULTITHREAD */
#  define DEBUG_PRIV(msg, ...) \
	syslog(LOG_DEBUG, msg"%s", __VA_ARGS__)
#endif /* MULTITHREAD */

/* ------------------------------------------------------------------------- */
#else /* SYSLOG */

#define ERROR_PRIV(msg, ...) if (1) { \
	if (errno) \
		fprintf (ERR_STREAM, msg"%s: %s\n", __VA_ARGS__, \
			strerror (errno)); \
	else \
		fprintf (ERR_STREAM, msg"%s\n", __VA_ARGS__); \
	} else (void)0

#define WARN_PRIV(msg, ...) \
	fprintf (ERR_STREAM, msg"%s\n", __VA_ARGS__)
#define INFO_PRIV(msg, ...) \
	fprintf (stdout, msg"%s\n", __VA_ARGS__)

#ifdef MULTITHREAD
#  define DEBUG_PRIV(msg, ...) \
	fprintf(DBG_STREAM, "Thread %p: "msg"%s\n", (void*)pthread_self(), \
		__VA_ARGS__)
#else /* MULTITHREAD */
#  define DEBUG_PRIV(msg, ...) \
	fprintf(DBG_STREAM, msg"%s\n", __VA_ARGS__)
#endif /* MULTITHREAD */

/* ------------------------------------------------------------------------- */
#endif /* SYSLOG */

#define ERROR(...) ERROR_PRIV(__VA_ARGS__, "")
#define WARN(...)  WARN_PRIV(__VA_ARGS__, "")
#define INFO(...)  INFO_PRIV(__VA_ARGS__, "")

#ifdef VERBOSE
#  define DEBUG(...) DEBUG_PRIV(__VA_ARGS__, "")
#else
#  define DEBUG(...)
#endif

#endif
