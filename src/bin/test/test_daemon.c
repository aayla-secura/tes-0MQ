#include "daemon.h"
#include <stdio.h>
#include <unistd.h>
#include <syslog.h>
#include <errno.h>

int main(void)
{
	int rc = daemonize();
	if (rc != 0)
	{
		puts("Couldn't go into background");
		if (errno)
			perror("");
		return -1;
	}
	syslog(LOG_USER | LOG_INFO, "foo here");
	sleep(10);
	syslog(LOG_USER | LOG_INFO, "foo done");
	return 0;
}
