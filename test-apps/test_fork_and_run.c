#include "daemon_ng.h"
#include <stdio.h>
#include <unistd.h>
#include <syslog.h>
#include <errno.h>

int foo (void* arg)
{
	syslog(LOG_DAEMON | LOG_INFO, "foo here");
	sleep(4);
	syslog(LOG_DAEMON | LOG_INFO, "foo done");
	return 0;
}

/* Check the system logger to confirm all is ok */
int main(void)
{
	int rc = fork_and_run(foo, foo, NULL, 5);
	if (rc != 0)
	{
		puts("Couldn't fork");
		if (errno)
			perror("");
		return -1;
	}
	syslog(LOG_DAEMON | LOG_INFO, "main here");
	sleep(8);
	syslog(LOG_DAEMON | LOG_INFO, "main done");
	return 0;
}
