/* pthread_[sg]etaffinity_np, CPU_SET and friends */
#ifdef linux
/* _GNU_SOURCE needed for pthread_[sg]etaffinity_np */
#  define _GNU_SOURCE
#  include <sched.h>
#  define cpuset_t cpu_set_t
#else
#  include <pthread_np.h>
#  include <sys/_cpuset.h>
#  include <sys/cpuset.h>
#endif

#include "cutil.h"
#include "daemon_ng.h"

#include <unistd.h>
#include <pthread.h>
#include <syslog.h>
#include <errno.h>

#ifndef NUMCPUS
#  define NUMCPUS 4L // fallback if sysconf
                     // (_SC_NPROCESSORS_ONLN) fails
#endif

int
pth_set_cpuaff (int cpu)
{
	pthread_t pt = pthread_self ();
	cpuset_t cpus;
	CPU_ZERO (&cpus);
	long ncpus = sysconf (_SC_NPROCESSORS_ONLN);
	if (ncpus == -1)
	{
		logmsg (errno, LOG_WARNING,
			"Cannot determine number of online cpus, "
			"using a fallback value of %ld", NUMCPUS);
		ncpus = NUMCPUS;
	}
	CPU_SET (cpu % (ncpus - 1), &cpus);
	int rc = pthread_setaffinity_np (pt, sizeof(cpuset_t), &cpus);
	if (rc == 0)
		rc = pthread_getaffinity_np (pt, sizeof(cpuset_t), &cpus);
	if (rc == 0)
	{
		for (long c = 0; c < ncpus; c++)
		{
			if ((CPU_ISSET (c, &cpus) && c != cpu) ||
				 (! CPU_ISSET (c, &cpus) && c == cpu))
			{
				rc = -1; /* unknown error */
				break;
			}
		}
	}
	/* errno is not set by pthread_*etaffinity_np, rc is the error */
	if (rc > 0)
		errno = rc;
	return (rc == 0 ? 0 : -1);
}
