/*
 * TODO:
 *   - use BSD's closefrom () if available 
 *   - method for finding highest fd number is not portable, see
 *     https://stackoverflow.com/questions/899038/getting-the-highest-allocated-file-descriptor/918469#918469
 * 
 * NOTES:
 *   - valgrind temporarily increases the current soft limit and
 *     opens some file descriptors, then brings it back down. If we
 *     iterate up to the hard limit, we run into trouble when
 *     running via valgrind. Use the soft limit instead
 */

#include "daemon_ng.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <syslog.h>
#include <sys/types.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

#define MAX_MSG_LEN         512
#define MIN_ERR_LEN          10
#define MAX_LOG_ID_LEN       32
#define MAX_LOG_TIMEFMT_LEN  16
#define MAX_LOG_TIME_LEN     64

static int verbose_level;
static char time_fmt[MAX_LOG_TIMEFMT_LEN];
static __thread char log_id[MAX_LOG_ID_LEN];

/* -------------------------------------------------------------- */
/* ----------------------------- API ---------------------------- */
/* -------------------------------------------------------------- */

void
logmsg (int errnum, int priority, const char* format, ...)
{
	if ( verbose_level <= priority - LOG_DEBUG )
		return;

	if (priority > LOG_DEBUG)
		priority = LOG_DEBUG;

	va_list args;
	va_start (args, format);
	char msg[MAX_MSG_LEN] = {0};
	int len = vsnprintf (msg, MAX_MSG_LEN, format, args);

	if (len < 0)
		return;

	if ( (len < MAX_MSG_LEN - 2 - MIN_ERR_LEN)  && errnum != 0 )
	{
		len += snprintf (msg + len, MAX_MSG_LEN - len, ": ");
		/* Thread-safe version of strerror. */
#if ((_POSIX_C_SOURCE >= 200112L) && !  _GNU_SOURCE) || ! linux
		/* POSIX compliant, returns int, saves it in msg */
		strerror_r (errnum, msg + len, MAX_MSG_LEN - len);
#else
		/* GNU version, returns it and MAY save it in msg */
		char* err = strerror_r (errnum,
			msg + len, MAX_MSG_LEN - len);
		if (err != NULL && err != msg + len)
			strncpy (msg + len, err, MAX_MSG_LEN - len);
#endif
	}

	char curtime[MAX_LOG_TIME_LEN] = {0};
	if (strlen (time_fmt) != 0)
	{
		time_t now = time(NULL);
		struct tm tm_now;
		struct tm* tm_now_p = localtime_r (&now, &tm_now);
		if (tm_now_p != NULL)
		{
			size_t wrc = strftime (curtime, MAX_LOG_TIME_LEN - 2,
				time_fmt, &tm_now);
			if (wrc == 0)
				curtime[0] = '\0'; /* error */
			else
				strncpy (curtime + wrc, ": ", MAX_LOG_TIME_LEN - wrc);
		}
	}
	if (ami_daemon ())
		syslog (priority, "%s%s%s", curtime, log_id, msg);
	else
	{
		FILE* outbuf;
		if ((priority == LOG_DEBUG) ||
			( verbose_level == 0 && priority < LOG_NOTICE ))
			outbuf = stderr;
		else
			outbuf = stdout;

		fprintf (outbuf, "%s%s%s\n", curtime, log_id, msg);
	}
	va_end (args);
}

char*
set_time_fmt (const char* fmt)
{
	if (fmt != NULL)
	{
		int rc = snprintf (time_fmt, MAX_LOG_TIMEFMT_LEN, "%s", fmt);
		if (rc >= MAX_LOG_TIMEFMT_LEN &&
			time_fmt[MAX_LOG_TIMEFMT_LEN - 1] == '%')
		{ /* got truncated in the middle of a format specifier */
			time_fmt[MAX_LOG_TIMEFMT_LEN - 1] = '\0';
		}
	}
	return time_fmt;
}

char*
set_logid (char* id)
{
	if (id != NULL)
		strncpy (log_id, id, MAX_LOG_ID_LEN);
	return log_id;
}

int
set_verbose (int level)
{
	if (level >= 0)
		verbose_level = level;
	return verbose_level;
}
