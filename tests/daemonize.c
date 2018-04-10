#include "daemon_ng.h"
#include <stdio.h>
#include <unistd.h>
#include <syslog.h>
#include <errno.h>

int foo (void* arg)
{
	logmsg (0, LOG_INFO, "foo here %d", getpid());
	sleep (2);
	logmsg (0, LOG_INFO, "foo done");
	return 0;
}

/* Check the system logger to confirm all is ok */
int main (void)
{
	for (int vlevel = 0; vlevel < 3; vlevel++)
	{
		logmsg (0, LOG_INFO, "Setting verbose level to %d", vlevel);
		set_verbose (vlevel);
		for (int l = 0; l < 3; l++)
			logmsg (0, LOG_DEBUG + l, "debug level %d", l);
	}

	int rc = daemonize ("/tmp/test.pid", foo, NULL, 5);
	if (rc != 0)
	{
		logmsg (0, LOG_ERR, "Couldn't go into background");
		if (errno)
			perror ("");
		return -1;
	}
	logmsg (0, LOG_INFO, "main here %d", getpid());
	sleep (2);
	logmsg (0, LOG_INFO, "main done");

	for (int vlevel = 0; vlevel < 3; vlevel++)
	{
		logmsg (0, LOG_INFO, "Setting verbose level to %d", vlevel);
		set_verbose (vlevel);
		for (int l = 0; l < 3; l++)
			logmsg (0, LOG_DEBUG + l, "debug level %d", l);
	}
	return 0;
}
