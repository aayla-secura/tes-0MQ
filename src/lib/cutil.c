/* CPU_SET and friends */
#ifdef linux
#  define _GNU_SOURCE
#  include <pthread.h>
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
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include <assert.h>
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

int
mkdirr (const char* path, mode_t mode)
{
	char finalpath[PATH_MAX] = {0};
	char* rp = canonicalize_path (NULL, path, finalpath, false,
		mode);
	if (rp == NULL)
		return -1;
	return 0;
}

char*
canonicalize_path (const char* root, const char* path,
	char* finalpath, bool mustexist, mode_t mode)
{
	assert (path != NULL);
	assert (finalpath != NULL);

	errno = 0;
	size_t len = strlen (path);
	if (len == 0)
	{
		logmsg (0, LOG_DEBUG, "Filename is empty");
		return NULL;
	}

	/*
	 * Make sure realroot starts and ends with a slash.
	 * It must end with a slash for memcmp to determine if inside
	 * realroot.
	 */
	char realroot[PATH_MAX] = {0};
	char buf[PATH_MAX] = {0};
	if (root == NULL || strlen (root) == 0 || root[0] != '/')
	{ /* prepend cwd if needed */
		char* rs = getcwd (buf, PATH_MAX);
		if (rs == NULL)
		{
			logmsg (errno, LOG_ERR,
				"Could not get current working directory");
			return NULL;
		}
		snprintf (realroot, PATH_MAX, "%s%s",
			buf, (buf[strlen (buf) - 1] == '/' ? "" : "/"));
	}
	if (root != NULL && strlen (root) > 0)
	{ /* add given root */
		snprintf (realroot, PATH_MAX, "%s%s%s", realroot, root,
				(root[strlen (root) - 1] == '/' ? "" : "/"));
	}

	int rc = snprintf (buf, PATH_MAX, "%s%s", realroot, path);
	if (rc >= PATH_MAX)
	{
		logmsg (0, LOG_DEBUG, "Filename too long");
		errno = ENAMETOOLONG;
		return NULL;
	}
	logmsg (0, LOG_DEBUG, "Canonicalizing path '%s'", buf);

	/* Check if the file exists first. */
	errno = 0;
	size_t rlen = strlen (realroot);
	char* rs = realpath (buf, finalpath);
	if (rs)
	{
		errno = 0;
		assert (rs == finalpath);
		if ( memcmp (finalpath, realroot, rlen) != 0 )
		{
			logmsg (0, LOG_DEBUG, "Resolved to %s, outside of root",
				finalpath);
			return NULL;
		}
		return finalpath;
	}
	if (mustexist)
	{
		logmsg (0, LOG_DEBUG, "File doesn't exist");
		return NULL;
	}

	/*
	 * We proceed only if some of the directories are missing, i.e.
	 * errno is ENOENT.
	 * errno is ENOTDIR only when a component of the parent path
	 * exists but is not a directory.
	 */
	if (errno != ENOENT)
		return NULL;

	/* Start from the top-most component (after realroot) and
	 * create directories as needed. */
	memset (&buf, 0, PATH_MAX);
	assert (rlen < PATH_MAX);
	strcpy (buf, realroot);

	const char* cur_seg = path;
	const char* next_seg = NULL;
	len = strlen (buf);
	while ( (next_seg = strchr (cur_seg, '/')) != NULL)
	{
		if (cur_seg[0] == '/')
		{ /* multiple consecutive slashes */
			cur_seg++;
			continue;
		}

		/* Copy leading slash of next_seg here, at the end. */
		assert (len < PATH_MAX);
		size_t thislen = next_seg - cur_seg + 1;
		if (len + thislen >= PATH_MAX)
		{
			logmsg (0, LOG_DEBUG, "Filename too long");
			errno = ENAMETOOLONG;
			return NULL;
		}
		strncpy (buf + len, cur_seg, thislen);
		len += thislen;
		assert (len == strlen (buf));

		logmsg (0, LOG_DEBUG, "Creating dir '%s'", buf);
		errno = 0;
		rc = mkdir (buf, mode);
		if (rc && errno != EEXIST)
			return NULL; /* don't handle other errors */

		cur_seg = next_seg + 1; /* skip over leading slash */
	}

	/* Canonicalize the directory part. */
	rs = realpath (buf, finalpath);
	assert (rs != NULL); /* this shouldn't happen */
	assert (rs == finalpath);
	
	len = strlen (finalpath); /* realpath removes the final slash */
	if (finalpath[len - 1] != '/')
	{
		snprintf (finalpath + len, PATH_MAX - len, "/");
		len++;
		assert (len == strlen (finalpath) || len == PATH_MAX);
	}

	if ( memcmp (finalpath, realroot, rlen) != 0)
	{
		logmsg (0, LOG_DEBUG, "Resolved to %s, outside of root",
			finalpath);
		return NULL;
	}
	
	if (strlen (cur_seg) == 0)
		return finalpath;

	/* Add the basename (realpath removes the trailing slash). */
	if (strlen (cur_seg) + len >= PATH_MAX)
	{
		logmsg (0, LOG_DEBUG, "Filename too long");
		errno = ENAMETOOLONG;
		return NULL;
	}

	snprintf (finalpath + len, PATH_MAX - len, "%s", cur_seg);
	return finalpath;
}
