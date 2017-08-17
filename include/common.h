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
#include <stdarg.h>
// #include <signal.h>
// #include <poll.h>
// #include <pthread.h>

// #define CZMQ_BUILD_DRAFT_API
#include <czmq.h>

#define FPGAPKT_DEBUG
#include <net/fpgapkt.h>

#include "daemon.h"

/* ------------------------------------------------------------------------- */

/*
 * The verbosity and the daemon state are taken from command-line options
 * passed when the server is started. They're saved in these external variables
 * and the methods for printing messages use the daemon's state.
 */

int is_daemon;
int is_verbose;

/*
 * Print fixed or formatted messages of a given priority (one of syslog's
 * levels.). If errnum is not 0, it will be included using strerror_r.  When
 * running as a daemon, messages are printed using syslog's corresponding
 * level. Facility is not changed, set it when opening syslog.
 *
 * If is_verbose is false, debugging messages are suppressed.
 * Otherwise, they are sent to stdout or stderr, depending on the verbosity level:
 * if is_verbose is false, warnings and errors are sent to stderr,
 * informational messages to stdout;
 * if is_verbose is true, debugging messages are sent to stderr, all others to
 * stdout.
 */

#define MAX_MSG_LEN 512

static void
s_msg (int errnum, int priority, int task, const char* msg)
{
	if ( ! is_verbose && priority == LOG_DEBUG )
		return;

	char err[MAX_MSG_LEN];
	memset (err, 0, MAX_MSG_LEN);
	if (errnum != 0)
	{
		int len = snprintf (err, MAX_MSG_LEN, ": ");
		/* Thread-safe version of strerror. */
		strerror_r (errnum, err + len, MAX_MSG_LEN - len);
	}

	if (is_daemon)
	{
		if (task > 0)
			syslog (priority, "Task #%d:    %s%s", task, msg, err);
		else
			syslog (priority, "Coordinator: %s%s", msg, err);
	}
	else if (( priority == LOG_DEBUG ) || ( ! is_verbose && priority < 5 ))
	{
		if (task > 0)
			fprintf (stderr, "Task #%d:    %s%s\n", task, msg, err);
		else
			fprintf (stderr, "Coordinator: %s%s\n", msg, err);
	}
	else
	{
		if (task > 0)
			fprintf (stdout, "Task #%d:    %s%s\n", task, msg, err);
		else
			fprintf (stdout, "Coordinator: %s%s\n", msg, err);
	}
}

static void
s_msgf (int errnum, int priority, int task, const char* format, ...)
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
		strerror_r (errnum, msg + len, MAX_MSG_LEN - len);
	}

	if (is_daemon)
	{
		if (task > 0)
			syslog (priority, "Task #%d: %s", task, msg);
		else
			syslog (priority, "Coordinator: %s", msg);
	}
	else if (( priority == LOG_DEBUG ) || ( ! is_verbose && priority < 5 ))
	{
		if (task > 0)
			fprintf (stderr, "Task #%d: %s\n", task, msg);
		else
			fprintf (stderr, "Coordinator: %s\n", msg);
	}
	else
	{
		if (task > 0)
			fprintf (stdout, "Task #%d: %s\n", task, msg);
		else
			fprintf (stdout, "Coordinator: %s\n", msg);
	}
	va_end (args);
}

/* ------------------------------------------------------------------------- */

/*
 * Dump packet (only in foreground and verbose mode).
 */

#define DUMP_ROW_LEN   16 /* how many bytes per row */
#define DUMP_OFF_LEN    5 /* how many digits to use for the offset */

static void
s_dump_pkt (const fpga_pkt* pkt)
{
	if ( ! is_verbose || is_daemon )
		return;

	u_int16_t len = pkt->length;
	const char* buf = (const char*)pkt;
	char line[ 4*DUMP_ROW_LEN + DUMP_OFF_LEN + 2 + 1 ];

	memset (line, 0, sizeof (line));
	for (int r = 0; r < len; r += DUMP_ROW_LEN) {
		sprintf (line, "%0*x: ", DUMP_OFF_LEN, r);

		/* hexdump */
		for (int b = 0; b < DUMP_ROW_LEN && b+r < len; b++)
			sprintf (line + DUMP_OFF_LEN + 2 + 3*b, "%02x ",
				(u_int8_t)(buf[b+r]));

		/* ASCII dump */
		for (int b = 0; b < DUMP_ROW_LEN && b+r < len; b++)
			sprintf (line + DUMP_OFF_LEN + 2 + b + 3*DUMP_ROW_LEN,
				"%c", isprint (buf[b+r]) ? buf[b+r] : '.');

		fprintf (stderr, "%s\n", line);
	}
	fprintf (stderr, "\n");
}

#endif
