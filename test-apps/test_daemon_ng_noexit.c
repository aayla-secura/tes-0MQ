#include "daemon_ng.h"
#include <stdio.h>
#include <unistd.h>
#include <syslog.h>
#include <errno.h>

/* Check the system logger to confirm all is ok */
int main(void)
{
	int fd = daemonize_noexit(NULL);
	if (fd == -1)
	{
		puts("Couldn't go into background");
		if (errno)
			perror("");
		return -1;
	}
	syslog(LOG_USER | LOG_INFO, "foo here");
	sleep(10);
	syslog(LOG_USER | LOG_INFO, "foo error");

	write (fd, DAEMON_ERR_MSG, 1);
	return -1;
}
