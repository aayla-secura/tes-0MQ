#include "daemon_ng.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <string.h>
#include <stdarg.h>
#include <paths.h>
#include <dirent.h>
#include <errno.h>
#include <assert.h>

#define MAX_MSG_LEN 512
#define LOG_ID_LEN 32

#define DAEMON_OK_MSG  "0"
#define DAEMON_ERR_MSG "1"
#define DAEMON_TIMEOUT 3000 /* deafault */

/*
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

static bool is_daemon;
static bool is_verbose;
static __thread char log_id[LOG_ID_LEN];

static rlim_t s_get_max_fd (void);
static void s_close_nonstd_fds (void);
static int s_close_open_fds (rlim_t max_fd);
static int s_wait_sig (int pipe_fd, int timeout_sec);
static int s_send_sig (int pipe_fd, char* sig);

/* ------------------------------------------------------------------------- */
/* -------------------------------- HELPERS -------------------------------- */
/* ------------------------------------------------------------------------- */

/*
 * Try to get the soft limit on fd numbers.
 */
static rlim_t
s_get_max_fd (void)
{
	struct rlimit rl;

	if (getrlimit (RLIMIT_NOFILE, &rl) == -1)
	{
		logmsg (0, LOG_DEBUG,
			"getrlimit returned -1, trying sysconf ()");
		return (rlim_t) sysconf (_SC_OPEN_MAX);
	}
	else
	{ /* return the soft, not hard, limit, see NOTES */
		return rl.rlim_cur;
	}
}

/*
 * Attempt to find and close all open file descriptors (except stdin, stdout,
 * stderr) up to the soft limit.
 * Returns 0 on success, -1 on error.
 */
static int
s_close_open_fds (rlim_t max_fd)
{
	DIR* self_fds_dir = NULL;
	struct dirent* self_fds_dirent;
	int dir_no;
	int dirent_no;
	int rc;

	/* /dev/fd should exist on both Linux and FreeBSD, otherwise check if
	 * procfs is enabled and provides it. */
	self_fds_dir = opendir ("/dev/fd");
	if (!self_fds_dir)
	{
		logmsg (0, LOG_DEBUG,
			"/dev/fd does not exist, trying /proc/self/fd");
		self_fds_dir = opendir ("/proc/self/fd");
	}
	if (!self_fds_dir)
	{
		logmsg (0, LOG_DEBUG, "/proc/self/fd does not exist");
		return -1;
	}

	/* Iterate through all fds in the directory and close all but stdin,
	 * stdout, stderr and the directory's fd. */
	dir_no = dirfd (self_fds_dir);
	errno = 0; /* reset for readdir */

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
		/* See NOTES for why this is needed. */
		if ((rlim_t)dirent_no >= max_fd)
			break;

		logmsg (0, LOG_DEBUG,
			"Closing fd = %d", dirent_no);
		rc = close (dirent_no);
		/* Should we return on error? */
		if (rc == -1) 
		{
			logmsg (errno, LOG_DEBUG, "close ()");
			/* return -1; */
		}

		errno = 0; /* reset it for readdir */
	}

	if (errno)
	{
		logmsg (errno, LOG_DEBUG, "readdir ()");
		return -1;
	}

	closedir (self_fds_dir);
	return 0;
}

/*
 * Closes all file descriptors (except stdin, stdout, stderr) up to the soft
 * limit. Calls s_close_open_fds.
 */
static void
s_close_nonstd_fds (void)
{
	rlim_t max_fd;
	rlim_t cur_fd;

	max_fd = s_get_max_fd ();
	logmsg (0, LOG_DEBUG,
		"s_get_max_fd () returned %li", max_fd);
	if (max_fd == 0)
	{
		logmsg (0, LOG_WARNING,
			"May not have closed all file descriptors. "
			"Could not get limit, so using 4096.");
		max_fd = 4096; /* be reasonable */
	}

	if (s_close_open_fds (max_fd) == 0)
		return;

	/* A fallback method: try to close all fd numbers up to some maximum. */
	logmsg (0, LOG_DEBUG, "Using fallback method");
	for (cur_fd = 0; cur_fd < max_fd; cur_fd++)
	{
		if (cur_fd == STDIN_FILENO)
			continue;
		if (cur_fd == STDOUT_FILENO)
			continue;
		if (cur_fd == STDERR_FILENO)
			continue;

		logmsg (0, LOG_DEBUG, "Closing fd = %li", cur_fd);
		close (cur_fd);
	}

	return;
}

/*
 * Read a signal from the pipe with the given timeout.
 * Returns 0 on DAEMON_OK_MSG, -1 on DAEMON_ERR_MSG.
 */
static int
s_wait_sig (int pipe_fd, int timeout_sec)
{
	struct pollfd poll_fd;
	poll_fd.fd = pipe_fd;
	poll_fd.events = POLLIN;

	int timeout = timeout_sec * 1000;
	if (timeout == 0)
		timeout = DAEMON_TIMEOUT;
	int rc = poll (&poll_fd, 1, timeout);

	if (rc == 0)
	{
		logmsg (0, LOG_ERR,
			"Timed out waiting for daemon to initialize");
		return -1;
	}
	if (rc == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not read from pipe");
		return -1;
	}

	char msg;
	ssize_t n = read (pipe_fd, &msg, 1);
	close (pipe_fd);

	if (n == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not read from pipe");
		return -1;
	}
	if (n != 1)
	{
		logmsg (0, LOG_ERR,
			"Read %lu bytes, expected 1", (size_t)n);
		return -1;
	}

	return ( memcmp (&msg, DAEMON_OK_MSG, 1) ? -1 : 0 );
}

/*
 * Send a signal to the pipe.
 * Returns 0 on success, -1 on error.
 */
static int
s_send_sig (int pipe_fd, char* sig)
{
	ssize_t n = write (pipe_fd, sig, 1);
	close (pipe_fd);

	if (n == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not write to pipe");
	}
	if (n != 1)
	{
		logmsg (0, LOG_ERR,
			"Wrote %lu bytes, expected 1",
			(size_t)n);
	}

	return ((n == 1) ? 0 : -1);
}

/* ------------------------------------------------------------------------- */
/* ---------------------------------- API ---------------------------------- */
/* ------------------------------------------------------------------------- */

void
logmsg (int errnum, int priority, const char* format, ...)
{
	if ( ! is_verbose && priority == LOG_DEBUG )
		return;

	va_list args;
	va_start (args, format);
	char msg[MAX_MSG_LEN];
	memset (msg, 0, MAX_MSG_LEN);
	int len = vsnprintf (msg, MAX_MSG_LEN, format, args);

	if ( (len < MAX_MSG_LEN - 10)  && errnum != 0 )
	{
		len += snprintf (msg + len, MAX_MSG_LEN - len, ": ");
		/* Thread-safe version of strerror. */
		/* FIX: XSI compliant or GNU version. */
		strerror_r (errnum, msg + len, MAX_MSG_LEN - len);
	}

	if (is_daemon)
		syslog (priority, "%s%s", log_id, msg);
	else
	{
		FILE* outbuf;
		if (( priority == LOG_DEBUG ) ||
				( ! is_verbose && priority < 5 ))
			outbuf = stderr;
		else
			outbuf = stdout;

		fprintf (outbuf, "%s%s\n", log_id, msg);
	}
	va_end (args);
}

char*
set_logid (char* id)
{
	if (id != NULL)
		strncpy (log_id, id, LOG_ID_LEN);
	return log_id;
}

int
set_verbose (int level)
{
	if (level >= 0)
		is_verbose = (level ? 1 : 0);
	return is_verbose;
	
}

int
daemonize (const char* pidfile, daemon_fn* initializer,
		void* arg, int timeout_sec)
{
	int rc;
	pid_t pid;

	/* Close all file descriptors except STDIN, STDOUT and STDERR */
	s_close_nonstd_fds ();

	/* Reset signal handlers and masks. */
	struct sigaction sa = {0,};
	sigemptyset (&sa.sa_mask);
	sigprocmask (SIG_UNBLOCK, &sa.sa_mask, NULL);
	sa.sa_handler = SIG_DFL;
	for ( int sig = 1; sig < NSIG ; sig++ )
	{
		errno = 0;
		sigaction (sig, &sa, NULL);
		if (errno)
			logmsg (0, LOG_DEBUG,
				"signal (%d, SIG_DFL)", sig);
	}

	/* We do not sanitize environment, that is the job of the caller. */

	/* Fork for the first time. */
	pid = fork ();

	if (pid == -1)
	{
		logmsg (errno, LOG_ERR, "Could not fork");
		return -1;
	}

	/* -------------------------------------------------- */
	/*                      Parent                        */
	/* -------------------------------------------------- */
	else if (pid > 0)
	{
		/* Wait for first child to exit. */
		waitpid (pid, &rc, 0);

		/* Parent is done. */
		if ( WEXITSTATUS (rc) != 0)
			return -1;

		exit (EXIT_SUCCESS);
	}

	/* -------------------------------------------------- */
	/*                   Child no. 1                      */
	/* -------------------------------------------------- */
	assert (pid == 0);

	/* Open a pipe to second child. */
	errno = 0;
	int pipe_fds[2];
	if ( pipe (pipe_fds) == -1 )
	{
		logmsg (errno, LOG_ERR,
			"Could not open a pipe to child");
		_exit (EXIT_FAILURE);
	}

	/* Detach from controlling TTY */
	pid = setsid ();
	if (pid == (pid_t)-1)
	{
		logmsg (errno, LOG_DEBUG, "setsid ()");
		_exit (EXIT_FAILURE);
	}
	
	/* Fork again to prevent daemon from obtaining a TTY */
	pid = fork ();

	if (pid == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not fork a second time");
		_exit (EXIT_FAILURE);
	}

	else if (pid > 0)
	{
		close (pipe_fds[1]); /* we don't use the write end */

		/* Wait for the second child initialize. */
		rc = s_wait_sig (pipe_fds[0], timeout_sec);

		/* Child number 1 is done. */
		_exit ( rc ? EXIT_FAILURE : EXIT_SUCCESS );
	}

	/* -------------------------------------------------- */
	/*                   Child no. 2                      */
	/* -------------------------------------------------- */
	assert (pid == 0);
	close (pipe_fds[0]); /* we don't use the read end */

	/* Clear umask. */
	umask (0);

	/* Change working directory. */
	rc = chdir ("/");
	if (rc == -1)
	{
		logmsg (errno, LOG_DEBUG, "chdir (\"/\")");
		s_send_sig (pipe_fds[1], DAEMON_ERR_MSG);

		_exit (EXIT_FAILURE);
	}

	/* Reopen STDIN, STDOUT and STDERR to /dev/null. */
	is_daemon = 1;
	rc = 0;
	if ( freopen (_PATH_DEVNULL, "r", stdin) == NULL )
		rc = -1;
	if ( rc == 0 && freopen (_PATH_DEVNULL, "w", stdout) == NULL )
		rc = -1;
	if ( rc == 0 && freopen (_PATH_DEVNULL, "w", stderr) == NULL )
		rc = -1;
	if (rc == -1)
	{
		/* Something went wrong, tell parent. */
		logmsg (errno, LOG_ERR,
			"Failed to reopen stdin, stdout or stderr");
		s_send_sig (pipe_fds[1], DAEMON_ERR_MSG);

		_exit (EXIT_FAILURE);
	}

	/* Write pid to a file. */
	if (pidfile)
	{
		int fd = open (pidfile, O_CREAT | O_WRONLY,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
		if (fd == -1)
		{
			logmsg (errno, LOG_ERR,
				"Failed to open pidfile %s",
				pidfile);
			s_send_sig (pipe_fds[1], DAEMON_ERR_MSG);

			_exit (EXIT_FAILURE);
		}
		
		char pid_s[12];
		pid = getpid();
		size_t pid_l = snprintf (pid_s, 12, "%u", pid);
		ssize_t n = write (fd, pid_s, pid_l);
		if (n == -1)
		{
			logmsg (errno, LOG_ERR,
				"Could not write to pidfile");
		}
		if ((size_t)n != pid_l)
		{
			logmsg (0, LOG_WARNING,
				"Wrote %lu bytes to pidfile, expected %lu",
				(size_t)n, pid_l);
		}

		logmsg (0, LOG_DEBUG,
			"Wrote pid (%u) to pidfile (%s)",
			pid, pidfile);
	}

	/* Call initializer. */
	if (initializer != NULL)
	{
		rc = initializer (arg);
		if (rc == -1)
		{
			logmsg (0, LOG_ERR,
				"Initializer encountered an error");
			s_send_sig (pipe_fds[1], DAEMON_ERR_MSG);

			_exit (EXIT_FAILURE);
		}
	}

	/* Done, signal child #1. */
	rc = s_send_sig (pipe_fds[1], DAEMON_OK_MSG);
	if (rc == -1)
		_exit (EXIT_FAILURE);

	closelog ();

	/* Return to caller. */
	return 0;
}

int
fork_and_run (daemon_fn* initializer, daemon_fn* action,
		void* arg, int timeout_sec)
{
	int rc;
	pid_t pid;

	/* Fork for the first time. */
	pid = fork ();

	if (pid == -1)
	{
		logmsg (errno, LOG_ERR, "Could not fork");
		return -1;
	}

	/* -------------------------------------------------- */
	/*                      Parent                        */
	/* -------------------------------------------------- */
	else if (pid > 0)
	{
		/* Wait for first child to exit. */
		waitpid (pid, &rc, 0);

		/* Parent is done. */
		return WEXITSTATUS (rc);
	}

	/* -------------------------------------------------- */
	/*                   Child no. 1                      */
	/* -------------------------------------------------- */
	assert (pid == 0);

	/* Open a pipe to second child. */
	errno = 0;
	int pipe_fds[2];
	if ( pipe (pipe_fds) == -1 )
	{
		logmsg (errno, LOG_ERR,
			"Could not open a pipe to child");
		_exit (EXIT_FAILURE);
	}

	/* Fork again to prevent the child becoming a zombie. */
	pid = fork ();

	if (pid == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not fork a second time");
		_exit (EXIT_FAILURE);
	}

	else if (pid > 0)
	{
		close (pipe_fds[1]); /* we don't use the write end */

		/* Wait for the second child initialize. */
		rc = s_wait_sig (pipe_fds[0], timeout_sec);

		/* Child number 1 is done. */
		_exit ( rc ? EXIT_FAILURE : EXIT_SUCCESS );
	}

	/* -------------------------------------------------- */
	/*                   Child no. 2                      */
	/* -------------------------------------------------- */
	assert (pid == 0);
	close (pipe_fds[0]); /* we don't use the read end */

	/* Call initializer. */
	if (initializer != NULL)
	{
		rc = initializer (arg);
		if (rc == -1)
		{
			logmsg (0, LOG_ERR,
				"Initializer encountered an error");
			s_send_sig (pipe_fds[1], DAEMON_ERR_MSG);

			_exit (EXIT_FAILURE);
		}
	}

	/* Done initializing, signal child #1. */
	rc = s_send_sig (pipe_fds[1], DAEMON_OK_MSG);
	if (rc == -1)
		_exit (EXIT_FAILURE);

	/* Perform task and exit. */
	if (action != NULL)
	{
		rc = action (arg);
		if (rc == -1)
		{
			logmsg (0, LOG_DEBUG,
				"Action encountered an error");

			_exit (EXIT_FAILURE);
		}
	}
	_exit (EXIT_SUCCESS);
}
