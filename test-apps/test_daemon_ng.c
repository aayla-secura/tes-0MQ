#include "daemon_ng.h"
#include <stdio.h>
#include <unistd.h>
#include <syslog.h>
#include <errno.h>

int foo (void* arg)
{
	syslog(LOG_DAEMON | LOG_INFO, "foo here");
	sleep(2);
	syslog(LOG_DAEMON | LOG_INFO, "foo done");
	return 0;
}

/* Check the system logger to confirm all is ok */
int main(void)
{
	int rc = daemonize_and_init("/tmp/test.pid", foo, NULL, 5);
	if (rc != 0)
	{
		puts("Couldn't go into background");
		if (errno)
			perror("");
		return -1;
	}
	syslog(LOG_DAEMON | LOG_INFO, "main here");
	sleep(2);
	syslog(LOG_DAEMON | LOG_INFO, "main done");
	return 0;
}
