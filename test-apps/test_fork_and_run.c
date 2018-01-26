#include "daemon_ng.h"
#include <stdio.h>
#include <unistd.h>
#include <syslog.h>
#include <errno.h>

int foo (void* arg)
{
	logmsg (0, LOG_INFO, "foo here %d", getpid());
	sleep (5);
	logmsg (0, LOG_INFO, "foo done");
	return 0;
}

/* Check the system logger to confirm all is ok */
int main (void)
{
	set_verbose (1);
	int rc = fork_and_run (foo, foo, NULL, 5);
	if (rc != 0)
	{
		logmsg (0, LOG_ERR, "Couldn't fork");
		if (errno)
			perror ("");
		return -1;
	}
	logmsg (0, LOG_INFO, "main here %d", getpid());
	sleep (10);
	logmsg (0, LOG_INFO, "main done");
	return 0;
}
