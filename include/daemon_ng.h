#ifndef __DAEMON_NG_H_INCLUDED__
#define __DAEMON_NG_H_INCLUDED__

/*
 * Daemonize process according to SysV specification:
 * https://www.freedesktop.org/software/systemd/man/daemon.html#SysV%20Daemons
 * 
 * Only tested on:
 *   - Linux > 4.8
 *   - FreeBSD 11.0
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
typedef int (daemon_init_fn)(void*);
int daemonize (const char* pidfile);
int daemonize_and_init (const char* pidfile, daemon_init_fn* initializer,
		void* arg, int timeout);
int fork_and_run (daemon_init_fn* initializer, daemon_init_fn* action,
		void* arg, int timeout_sec);

#endif
