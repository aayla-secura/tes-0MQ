#include "cutil.h"
#include "daemon_ng.h"

#include <syslog.h>
#include <errno.h>

void
tic (struct timespec* ts)
{
	clock_gettime (CLOCK_REALTIME, ts);
}

long long
toc (struct timespec* ts)
{
	struct timespec te;
	int rc = clock_gettime (CLOCK_REALTIME, &te);
	if (rc == -1)
	{
		logmsg (errno, LOG_ERR, "Cannot get CLOCK_REALTIME");
		return -1;
	}
	te.tv_sec -= ts->tv_sec;
	te.tv_nsec -= ts->tv_nsec;

	return (long long)te.tv_sec * NSEC_IN_SEC + te.tv_nsec;
}
