#ifndef __DAEMON_NG_H_INCLUDED__
#define __DAEMON_NG_H_INCLUDED__

#include <sys/types.h>
#include <stdbool.h>

/*
 * Forking and logging functions.
 */

typedef int (daemon_fn)(void*);

/*
 * Daemonize process according to SysV specification:
 * https://www.freedesktop.org/software/systemd/man/daemon.html#SysV%20Daemons
 *
 * on success (failure) daemonize will:
 *    close all file descriptors
 *    fork
 *     |-> setsid -> fork
 *     |              |-> init -> signal first fork -> return (exit)
 *     |             exit <----------------|
 *    wait <----------|
 *     |
 *    exit (return)
 * 
 * If second fork succeeds, clear the umask, change the working
 * directory to /, call initializer (unless NULL) passing
 * it arg. If initializer returns 0, close stdin, stdout, stderr,
 * write pid to file.

 * If we fail at any point, the daemon exits and the parent returns
 * with -1.
 * Otherwise parent exits with 0 and the daemon returns 0 to caller.
 *
 * The parent will wait for the initializer up to timeout seconds.
 * If timeout is 0, it defaults to 3 seconds.
 * If timeout is < 0, parent waits forever.
 * 
 * If pidfile != NULL, write daemon pid there.
 */
int daemonize (const char* pidfile, daemon_fn* initializer,
		void* arg, int timeout);

/*
 * Run a task in a fork and exit. Does not close open descriptors or
 * detach from terminal. Does a double fork, so second child will
 * not be a zombie.
 *   
 * fork_and_run will:
 *    fork
 *     |-> fork
 *     |     |-> init -> signal first fork -> run action -> exit
 *     |    exit <----------------|
 *    wait <-|
 *     |
 *    return
 *
 * If second fork succeeds, call initializer (unless NULL) passing
 * it arg. If initializer returns 0, the parent returns with 0, the
 * child executes action, passing it same arg, and exits.
 *
 * If we fail before calling initializer, or if initializer fails,
 * child exits and parent returns with -1.
 *
 * The parent will wait for the initializer up to timeout seconds.
 * If timeout is 0, it defaults to 3 seconds.
 * If timeout is < 0, parent waits forever.
 */
int fork_and_run (daemon_fn* initializer, daemon_fn* action,
		void* arg, int timeout_sec);

/*
 * Drop privileges of the current process. It calls setuid and setgid
 * and if the calling process was privileged, makes sure it is not
 * able to regain privileges.
 * Returns 0 on success, -1 on error.
 */
int run_as (uid_t uid, gid_t gid);

/* -------------------------------------------------------------- */

/*
 * Print formatted messages of a given priority (one of syslog's
 * levels.). If errnum is not 0, error description will be appended.
 *
 * priority may be > LOG_DEBUG (numerically highest valid syslog
 * level), in which case it is treated as LOG_DEBUG but the process'
 * verbosity level determins if the message is printed at all.
 *
 * When running as a daemon all messages are sent to syslog with the
 * requested priority.
 * When running in foreground they are sent to either stdout or
 * stderr, depending on the verbosity level:
 *   if verbosity level == 0, warnings and errors are sent to stderr,
 *     other messages to stdout;
 *   if verbosity level > 0, debugging messages are sent to stderr,
 *     all others to stdout.
 *
 * Messages of priority = LOG_DEBUG + n are always suppressed if
 * verbosity level is <= n. I.e. verbosity = 0 suppresses all
 * debugging messages.
 *
 * Messages may be prefixed by the current time, if a time format is set
 * via set_time_fmt.
 *
 * Messages are prefixed by <logid>, which is thread-specific and
 * set via set_logid.
 *
 * If prefix + formatted message + error is longer than 512
 * characters, it is truncated.
 */
void logmsg (int errnum, int priority, const char* format, ...);

/*
 * Set or get log time_format.
 * If fmt is not NULL, set the time format. If the format is longer than
 * 16 characters, it is truncated. If resulting time string is longer
 * than 62 bytes, it is truncated.
 * Returns a pointer to the currently set time format.
 */
char* set_time_fmt (const char* fmt);

/*
 * Set or get <logid>.
 * If id is not NULL, set the log prefix for the calling thread. If
 * it is longer than 32 characters, it is truncated.
 * Returns a pointer to the thread-local static string containing
 * the prefix.
 */
char* set_logid (char* id);

/*
 * Set or get verbosity level. It is not thread-specific.
 * If be_verbose < 0, value is unchanged.
 * Returns the set value.
 */
int set_verbose (int level);

/*
 * Returns 1 is process has been daemonized (using damonize).
 * Returns 0 otherwise.
 */
bool ami_daemon (void);

#endif
