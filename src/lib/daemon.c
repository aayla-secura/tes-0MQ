#include "daemon.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <string.h>
#include <paths.h>
#include <dirent.h>
#include <errno.h>

#define DAEMON_OK_MSG "0"
#define DAEMON_ERR_MSG "1"
#define DAEMON_TIMEOUT 3000

#ifdef VERBOSE
#  define DEBUG(...) \
	syslog (LOG_DAEMON | LOG_DEBUG, __VA_ARGS__)
#else
#  define DEBUG(...)
#endif
#define ERROR(...) \
	syslog (LOG_DAEMON | LOG_ERR, __VA_ARGS__)
#define WARN(...) \
	syslog (LOG_DAEMON | LOG_WARNING, __VA_ARGS__)

/*
 * Daemonize process according to SysV specification:
 * https://www.freedesktop.org/software/systemd/man/daemon.html#SysV%20Daemons
 * 
 * Only tested on:
 *   - Linux 4.8
 * 
 * TO DO:
 *   - use BSD's closefrom () if available 
 *   - method for finding highest fd number is not portable, see
 *     https://stackoverflow.com/questions/899038/getting-the-highest-allocated-file-descriptor/918469#918469
 *   - implement optional dropping of privileges
 * 
 * NOTES:
 *   - valgrind temporarily increases the current soft limit and opens some file
 *     descriptors, then brings it back down. If we iterate up to the hard
 *     limit, we run into trouble when running via valgrind. Use the soft limit
 *     instead
 */

static rlim_t s_get_max_fd (void);
static void s_close_nonstd_fds (void);
static int s_close_open_fds (rlim_t max_fd);

/* ------------------------------------------------------------------------- */
/* -------------------------------- STATIC --------------------------------- */
/* ------------------------------------------------------------------------- */

/* Try to get the soft limit on fd numbers  */
static rlim_t
s_get_max_fd (void)
{
	struct rlimit rl;

	if (getrlimit (RLIMIT_NOFILE, &rl) == -1)
	{
		DEBUG ("getrlimit returned -1, trying sysconf ()");
		return (rlim_t) sysconf (_SC_OPEN_MAX);
	}
	else
		return rl.rlim_cur; /* Return the soft, not hard, limit, see NOTES */
}

/* Attempt to find all open file descriptors instead of blindly iterating up to
 * the maximum fd number */
static int
s_close_open_fds (rlim_t max_fd)
{
	DIR* self_fds_dir = NULL;
	struct dirent* self_fds_dirent;
	int dir_no;
	int dirent_no;
	int rc;

	/* /dev/fd should exist on both Linux and FreeBSD, otherwise check if
	 * procfs is enabled and provides it */
	self_fds_dir = opendir ("/dev/fd");
	if (!self_fds_dir)
	{
		DEBUG ("/dev/fd does not exist, trying /proc/self/fd");
		self_fds_dir = opendir ("/proc/self/fd");
	}
	if (!self_fds_dir)
	{
		DEBUG ("/proc/self/fd does not exist");
		return -1;
	}

	/* Iterate through all fds in the directory and close all but stdin,
	 * stdout, stderr and the directory's fd */
	dir_no = dirfd (self_fds_dir);
	errno = 0; /* Reset for readdir */

	while ( (self_fds_dirent = readdir (self_fds_dir)) )
	{
		dirent_no = atoi (self_fds_dirent->d_name);

		if ( !strcmp (self_fds_dirent->d_name, ".") )
			continue;
		if ( !strcmp (self_fds_dirent->d_name, "..") )
			continue;
		if (dirent_no == dir_no)
			continue;
		if (dirent_no == STDIN_FILENO)
			continue;
		if (dirent_no == STDOUT_FILENO)
			continue;
		if (dirent_no == STDERR_FILENO)
			continue;
		if ((rlim_t)dirent_no >= max_fd)
			break;

		DEBUG ("Closing fd = %d", dirent_no);
		rc = close (dirent_no);
		/* Should we return on error? */
		if (rc == -1) 
		{
			DEBUG ("%m");
			/* return -1; */
		}

		errno = 0; /* Reset it for readdir */
	}

	if (errno)
	{
		DEBUG ("readdir (): %m");
		return -1; /* Something went wrong... */
	}

	closedir (self_fds_dir);
	return 0;
}

/* Attempts to close all file descriptors (except stdin, stdout, stderr) up to
 * the soft limit  */
static void
s_close_nonstd_fds (void)
{
	rlim_t max_fd;
	rlim_t cur_fd;

	max_fd = s_get_max_fd ();
	DEBUG ("s_get_max_fd () returned %li", max_fd);
	if (max_fd == 0)
	{
		DEBUG ("Using 4096 as the maximum fdno then");
		WARN ("May not have closed all file descriptors. "
			"Could not get limit, so using 4096.");
		max_fd = 4096; /* Be reasonable */
	}

	if (s_close_open_fds (max_fd) == 0)
		return;

	/* A fallback method: try to close all fd numbers up to some maximum */
	DEBUG ("Using fallback method");
	for (cur_fd = 0; cur_fd < max_fd; cur_fd++)
	{
		if (cur_fd == STDIN_FILENO)
			continue;
		if (cur_fd == STDOUT_FILENO)
			continue;
		if (cur_fd == STDERR_FILENO)
			continue;

		DEBUG ("Closing fd = %li", cur_fd);
		close (cur_fd);
	}

	return;
}

/* ------------------------------------------------------------------------- */
/* ------------------------------- EXTERNAL -------------------------------- */
/* ------------------------------------------------------------------------- */

/* If we fail, we return -1 from the parent (i.e. in foreground) to the caller,
 * otherwise parent exits with 0 and the daemon returns 0 to caller. */
int
daemonize (const char* pidfile)
{
	int rc;
	int sig;
	pid_t pid;
	int pipe_fds[2];

	/* Close all file descriptors except STDIN, STDOUT and STDERR */
	s_close_nonstd_fds ();

	/* Reset signal handlers and masks */
	struct sigaction sa = {0,};
	sigemptyset (&sa.sa_mask);
	sigprocmask (SIG_UNBLOCK, &sa.sa_mask, NULL);
	sa.sa_handler = SIG_DFL;
	for ( sig = 1; sig < NSIG ; sig++ )
	{
		errno = 0;
		sigaction (sig, &sa, NULL);
		if (errno)
		{
			DEBUG ("signal (%d, SIG_DFL): %m", sig);
		}
	}

	/* We do not sanitize environment, that is the job of the caller */

	/* Open a pipe for parent to second child communication */
	errno = 0;
	if ( pipe (pipe_fds) == -1 )
	{
		ERROR ("Could not open a pipe to communicate with fork");
		return -1;
	}

	/* -------------------------------------------------- */
	/*             Fork for the first time                */
	/* -------------------------------------------------- */
	pid = fork ();

	if (pid == -1)
	{
		ERROR ("Could not fork");
		return -1;
	}

	/* -------------------------------------------------- */
	/*                      Parent                        */
	/* -------------------------------------------------- */
	else if (pid > 0)
	{
		char sig;
		struct pollfd poll_fd;
		poll_fd.fd = pipe_fds[0];
		poll_fd.events = POLLIN;

		/* Wait for the second child (the daemon) */
		close (pipe_fds[1]); /* We don't use the write end */

		/* Set a reasonable timeout */
		rc = poll (&poll_fd, 1, DAEMON_TIMEOUT);

		if (rc == 0)
		{
			/* Timed out */
			ERROR ("Timed out waiting for daemon to initialize");
			close (pipe_fds[0]);
			return -1;
		}
		if (rc == -1)
		{
			/* Something went wrong, assume daemon is dead or never
			 * started */
			ERROR ("Could not read from pipe: %m");
			close (pipe_fds[0]);
			return -1;
		}

		rc = read (pipe_fds[0], &sig, 1);
		close (pipe_fds[0]);
		if ( memcmp (&sig, DAEMON_OK_MSG, 1) != 0 )
		{
			/* Second fork didn't happen or failed and exited */
			DEBUG ("Read an error from pipe");
			return -1;
		}

		/* Parent is done */
		exit (EXIT_SUCCESS);
	}

	/* -------------------------------------------------- */
	/*                   Child no. 1                      */
	/* -------------------------------------------------- */
	else
	{
		close (pipe_fds[0]); /* We don't use the read end */

		/* Detach from controlling TTY */
		pid = setsid ();
		if (pid == (pid_t)-1)
		{
			/* Something went wrong, tell parent */
			ERROR ("setsid (): %m");
			rc = write (pipe_fds[1], DAEMON_ERR_MSG, 1);
			close (pipe_fds[1]);

			_exit (EXIT_FAILURE);
		}
		
		/* Fork again to prevent daemon from obtaining a TTY */
		pid = fork ();

		if (pid == -1)
		{
			/* Something went wrong, tell parent */
			ERROR ("Could not fork a second time");
			rc = write (pipe_fds[1], DAEMON_ERR_MSG, 1);
			close (pipe_fds[1]);

			_exit (EXIT_FAILURE);
		}

		else if (pid > 0)
		{
			/* Child number 1 is done */
			close (pipe_fds[1]);

			_exit (EXIT_SUCCESS);
		}

	/* -------------------------------------------------- */
	/*                   Child no. 2                      */
	/* -------------------------------------------------- */
		else
		{
			/* Clear umask */
			umask (0);

			/* Change working directory */
			rc = chdir ("/");
			if (rc == -1)
			{
				/* Something went wrong, tell parent */
				ERROR ("chdir (\"/\"): %m");
				rc = write (pipe_fds[1], DAEMON_ERR_MSG, 1);
				close (pipe_fds[1]);

				_exit (EXIT_FAILURE);
			}

			/* Reopen STDIN, STDOUT and STDERR to /dev/null */
			rc = 0;
			if ( freopen (_PATH_DEVNULL, "r", stdin) == NULL )
				rc = -1;
			if ( rc == 0 && freopen (_PATH_DEVNULL, "w", stdout) == NULL )
				rc = -1;
			if ( rc == 0 && freopen (_PATH_DEVNULL, "w", stderr) == NULL )
				rc = -1;
			if (rc == -1)
			{
				/* Something went wrong, tell parent */
				ERROR ("freopen (%s, ...): %m", _PATH_DEVNULL);
				ERROR ("Failed to reopen stdin, stdout or stderr");
				rc = write (pipe_fds[1], DAEMON_ERR_MSG, 1);
				close (pipe_fds[1]);

				_exit (EXIT_FAILURE);
			}

			/* Write pid to a file */
			if (pidfile)
			{
				int fd = open (pidfile, O_CREAT | O_WRONLY,
					S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
				if (fd == -1)
				{
					ERROR ("Failed to open pidfile %s: %m",
						pidfile);
					rc = write (pipe_fds[1], DAEMON_ERR_MSG, 1);
					close (pipe_fds[1]);

					_exit (EXIT_FAILURE);
				}
				
				char pid_s[12];
				pid = getpid();
				int pid_l = snprintf (pid_s, 12, "%u", pid);
				rc = write (fd, pid_s, pid_l);
				DEBUG ("Wrote pid (%u) to pidfile (%s)",
					pid, pidfile);
			}

			/* Done, signal parent */
			rc = write (pipe_fds[1], DAEMON_OK_MSG, 1);
			close (pipe_fds[1]);
			closelog ();

			/* Return to caller */
			return 0;
		}
	}
}
