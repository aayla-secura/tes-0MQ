#ifndef __DAEMON_NG_H_INCLUDED__
#define __DAEMON_NG_H_INCLUDED__

/*
 * Forking and logging functions.
 *
 * Only tested on:
 *   - Linux > 4.8
 *   - FreeBSD 11.0
 */

typedef int (daemon_fn)(void*);

/*
 * Daemonize process according to SysV specification:
 * https://www.freedesktop.org/software/systemd/man/daemon.html#SysV%20Daemons
 *
 * daemonize_and_init will:
 *    close all file descriptors
 *    fork
 *     |--> setsid --> fork
 *     |                |--> init --> signal parent --> return to caller
 *    exit <-----------------------------|
 * 
 * If second fork succeeds, call initializer (unless NULL) passing it arg. If
 * initializer returns 0, parent exits with 0 and the daemon returns 0 to caller.

 * If we fail before calling initializer, or if initializer fails, we return -1
 * from the parent (i.e. in foreground) to the caller.
 *
 * The parent will wait for the initializer up to timeout seconds.
 * If timeout is 0, it defaults to 3 seconds.
 * If timeout is < 0, parent waits forever.
 * 
 * If pidfile != NULL, write pid there.
 */
int daemonize (const char* pidfile, daemon_fn* initializer,
		void* arg, int timeout);

/*
 * Run a task in a fork and exit. Does not close open descriptors or detach
 * from terminal. Does a double fork, so second child will not be a zombie.
 *   
 * a fork_and_run will:
 *    fork
 *     |---> fork
 *     |      |--> init --> signal first fork --> run action --> exit
 *     |     exit <------------------|
 *    wait <--|
 *     |
 *    return to caller
 *
 * If second fork succeeds, call initializer (unless NULL) passing it arg. If
 * initializer returns 0, parent returns with 0, the child executes action,
 * passing it same arg, and exits.
 *
 * If we fail before calling initializer, or if initializer fails, we return -1
 * from the parent to the caller.
 *
 * The parent will wait for the initializer up to timeout seconds.
 * If timeout is 0, it defaults to 3 seconds.
 * If timeout is < 0, parent waits forever.
 */
int fork_and_run (daemon_fn* initializer, daemon_fn* action,
		void* arg, int timeout_sec);

/* ------------------------------------------------------------------------- */

/*
 * Print formatted messages of a given priority (one of syslog's levels.). If
 * errnum is not 0, it will be included using strerror_r.
 *
 * When running as a daemon all messages are sent to syslog with the requested
 * priority.
 * When running in foreground they are sent to either stdout or stderr,
 * depending on the verbosity level:
 *   if verbose is false, warnings and errors are sent to stderr,
 *     informational messages to stdout;
 *   if verbose is true, debugging messages are sent to stderr, all others
 *     to stdout.
 *
 * If verbose is false, debugging messages are always suppressed.
 *
 * Messages are prefixed by <logid>, which is thread-specific and set via
 * set_logid.
 *
 * If prefix + formatted message + error is longer than 512 characters, it is
 * truncated.
 */
void logmsg (int errnum, int priority, const char* format, ...);

/*
 * If id is not NULL, set the log prefix for the calling thread. If it is
 * longer than 32 characters, it is truncated.
 * Returns a pointer to the thread-local static string containing the prefix.
 */
char* set_logid (char* id);

/*
 * If is_verbose < 0 the current value is returned.
 * If is_verbose == 0, debugging messages are suppressed, 0 is returned.
 * If be_verbose > 0, they will be printed, 1 is returned.
 */
int set_verbose (int be_verbose);

#endif
